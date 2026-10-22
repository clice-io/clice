module;

#include "modules/prelude.h"

module clice;

import :compile.compilation;
import :compile.directive;
import :feature.documentation;
import :feature.signature_help;
import :semantic.decls;
import :semantic.display;
import :semantic.resolver;
import :semantic.types;
import :support.markup;
import :syntax.documentation;
import :syntax.lexer;
import :syntax.lexical_scan;

namespace clice::feature {

namespace lsp = kota::ipc::lsp;

namespace {

/// The token written right before `location`; none when a macro wrote
/// what is there.
std::optional<clang::Token> token_before(clang::Sema& sema, clang::SourceLocation location) {
    auto& sources = sema.getSourceManager();
    if(sources.isMacroArgExpansion(location)) {
        location = sources.getSpellingLoc(location);
    }
    if(!location.isFileID()) {
        return std::nullopt;
    }
    return clang::Lexer::findPreviousToken(location,
                                           sources,
                                           sema.getLangOpts(),
                                           /*IncludeComments=*/false);
}

/// The name a call through an object goes by: the identifier written
/// before its opening parenthesis (`square` in `square(`), empty when
/// another expression is called (`make()(`).
std::string callee_name(clang::Sema& sema, clang::SourceLocation open_paren) {
    auto token = token_before(sema, open_paren);
    if(!token || !token->is(clang::tok::raw_identifier)) {
        return {};
    }
    return token->getRawIdentifier().str();
}

/// Whether a member call writes its object, `b.at(` or `p->at(`: Sema
/// ranks overloads by the object only then. An implicit `this` takes no
/// part, which leaves a const and a non-const overload tied.
bool object_written(clang::Sema& sema, clang::SourceLocation open_paren) {
    auto name = token_before(sema, open_paren);
    if(!name || !name->is(clang::tok::raw_identifier)) {
        return false;
    }
    auto access = token_before(sema, name->getLocation());
    return access && access->isOneOf(clang::tok::period, clang::tok::arrow);
}

/// Whether `method` differs from `better`, a method of the same class Sema
/// ranked above it, only in its qualifiers: with the same parameters it is
/// never the one a call picks, as the const overload on a non-const object.
/// A function template is an overload of its own even where a
/// specialization takes the same parameters.
bool dominated(const clang::FunctionDecl* method, const clang::FunctionDecl* better) {
    auto plain_method = [](const clang::FunctionDecl* function) {
        return llvm::isa<clang::CXXMethodDecl>(function) && !function->getPrimaryTemplate() &&
               !function->getDescribedFunctionTemplate();
    };
    if(!plain_method(method) || !plain_method(better) ||
       method->getParent() != better->getParent() ||
       method->getNumParams() != better->getNumParams() ||
       method->isVariadic() != better->isVariadic()) {
        return false;
    }
    auto& context = method->getASTContext();
    return std::ranges::all_of(std::views::zip(method->parameters(), better->parameters()),
                               [&](auto pair) {
                                   auto [lhs, rhs] = pair;
                                   return context.hasSameType(lhs->getType(), rhs->getType());
                               });
}

/// The declaration a candidate shows as: a function template's
/// specialization as the template declares it, unless written as one of
/// its own; a member of a class template's specialization as the class
/// instantiates it.
const clang::FunctionDecl* shown(const clang::FunctionDecl* function) {
    if(auto* primary = function->getPrimaryTemplate();
       primary && function->getTemplateSpecializationKind() != clang::TSK_ExplicitSpecialization) {
        return primary->getTemplatedDecl();
    }
    return function;
}

/// Puts a class's implicit constructors, which Sema ranks alike with the
/// declared ones for a call with no argument written yet, after those, in
/// the order Sema declares them: Sema leaves them where its lookup found
/// them, which depends on when something first needed one, in a
/// preamble's PCH or later in the file. Constructor templates, which Sema
/// ranks after both, stay last.
void place_implicit(
    llvm::MutableArrayRef<clang::CodeCompleteConsumer::OverloadCandidate> candidates) {
    auto implicit = [](const clang::CodeCompleteConsumer::OverloadCandidate& candidate) {
        const auto* function = candidate.getFunction();
        return function && function->isImplicit() && llvm::isa<clang::CXXConstructorDecl>(function);
    };
    if(std::ranges::none_of(candidates, implicit)) {
        return;
    }
    auto rank = [](const clang::CodeCompleteConsumer::OverloadCandidate& candidate) {
        const auto* function = candidate.getFunction();
        if(function &&
           (function->getPrimaryTemplate() || function->getDescribedFunctionTemplate())) {
            return 4;
        }
        const auto* constructor = llvm::dyn_cast_or_null<clang::CXXConstructorDecl>(function);
        if(!constructor || !constructor->isImplicit()) {
            return 0;
        }
        return constructor->isDefaultConstructor() ? 1
               : constructor->isCopyConstructor()  ? 2
               : constructor->isMoveConstructor()  ? 3
                                                   : 0;
    };
    std::ranges::stable_sort(candidates, [&](const auto& left, const auto& right) {
        return rank(left) < rank(right);
    });
}

/// The parameters an instantiation expanded from a pack of the
/// declaration it was instantiated from, as indices [begin, end): each
/// is named after the pack, which names none of them. A member template
/// of a class template's specialization keeps its own pack.
std::pair<unsigned, unsigned> expanded_pack(const clang::FunctionDecl& function) {
    auto* pattern = decls::function_pattern(function);
    if(!pattern) {
        return {};
    }
    auto params = pattern->parameters();
    auto pack = std::ranges::find_if(params, [](auto* param) { return param->isParameterPack(); });
    if(pack == params.end()) {
        return {};
    }
    unsigned begin = pack - params.begin();
    if(begin < function.getNumParams() && function.getParamDecl(begin)->isParameterPack()) {
        return {};
    }
    return {begin, begin + function.getNumParams() + 1 - params.size()};
}

/// One signature as it is built: its label and the range of each
/// parameter in it.
struct Signature {
    llvm::SmallString<128> label;
    llvm::raw_svector_ostream stream{label};
    std::vector<protocol::ParameterInformation> parameters;

    /// The names a documentation comment may give each parameter, the one
    /// the signature shows first; none for one it cannot name (an expanded
    /// pack's element).
    std::vector<llvm::SmallVector<std::string, 1>> names;

    /// The last parameter takes every argument past the ones before it: a
    /// pack or a C-style `...`.
    bool pack = false;

    /// Writes one parameter, which a comment may name `names`, with `print`.
    void parameter(llvm::function_ref<void(llvm::raw_ostream&)> print,
                   llvm::SmallVector<std::string, 1> names = {}) {
        if(!parameters.empty()) {
            stream << ", ";
        }
        auto begin = lsp::encoded_length(label.str(), PositionEncoding::UTF16);
        print(stream);
        auto end = lsp::encoded_length(label.str(), PositionEncoding::UTF16);
        parameters.push_back({
            .label = std::tuple<protocol::uinteger, protocol::uinteger>{begin, end},
        });
        this->names.push_back(std::move(names));
    }

    /// The finished signature, the parameter `argument` falls on active.
    protocol::SignatureInformation finish(std::uint32_t argument) && {
        if(pack && argument >= parameters.size()) {
            argument = static_cast<std::uint32_t>(parameters.size() - 1);
        }
        protocol::SignatureInformation signature{.label = label.str().str()};
        if(!parameters.empty()) {
            signature.parameters = std::move(parameters);
        }
        signature.active_parameter = protocol::nullable<protocol::uinteger>(argument);
        return signature;
    }
};

/// What documents a signature: a declaration, and the names of its
/// parameters (Signature::names).
struct Documented {
    const clang::NamedDecl* decl = nullptr;
    std::vector<llvm::SmallVector<std::string, 1>> parameters;
};

class Builder final : public clang::CodeCompleteConsumer {
public:
    Builder(protocol::SignatureHelp& help,
            std::vector<Documented>& sources,
            clang::CodeCompleteOptions complete_options) :
        clang::CodeCompleteConsumer(complete_options), help(help), sources(sources),
        info(std::make_shared<clang::GlobalCodeCompletionAllocator>()) {}

    void ProcessOverloadCandidates(clang::Sema& sema,
                                   std::uint32_t current_arg,
                                   OverloadCandidate* candidates,
                                   std::uint32_t candidate_count,
                                   clang::SourceLocation open_paren,
                                   bool braced) final {
        if(forwarding) {
            // The standard library reaches only public constructors.
            for(auto& candidate: std::ranges::subrange(candidates, candidates + candidate_count)) {
                auto* constructor = candidate.getFunction();
                if(constructor->getAccess() == clang::AS_public) {
                    add_forwarded(*shown(constructor), [&](Signature& signature) {
                        arguments(signature, *shown(constructor));
                    });
                }
            }
            return;
        }

        help.signatures.reserve(candidate_count);
        start(sema.getASTContext());
        call_scope = sema.CurContext;

        bool ranked = object_written(sema, open_paren);
        if(current_arg == 0) {
            place_implicit({candidates, candidate_count});
        }
        llvm::SmallVector<const clang::FunctionDecl*> listed;
        for(auto& candidate: std::ranges::subrange(candidates, candidates + candidate_count)) {
            if(auto* function = candidate.getFunction()) {
                if(ranked && std::ranges::any_of(listed, [&](auto* better) {
                       return dominated(function, better);
                   })) {
                    continue;
                }
                listed.push_back(function);
                if(!forwarded(sema, *function, current_arg, open_paren)) {
                    // A call through an object reads as a call of a function
                    // named as the object.
                    const auto* method = llvm::dyn_cast<clang::CXXMethodDecl>(function);
                    auto name = method && method->getOverloadedOperator() == clang::OO_Call
                                    ? callee_name(sema, open_paren)
                                    : std::string();
                    call(*function, current_arg, braced, name);
                }
                continue;
            }

            Signature signature;
            auto& stream = signature.stream;
            switch(candidate.getKind()) {
                case clang::CodeCompleteConsumer::OverloadCandidate::CK_FunctionType: {
                    auto type = candidate.getFunctionType();
                    stream << "(";
                    if(auto* proto = llvm::dyn_cast<clang::FunctionProtoType>(type)) {
                        for(auto param_type: proto->param_types()) {
                            signature.parameter(
                                [&](llvm::raw_ostream& os) { print(os, param_type, call_scope); });
                        }
                        variadic(signature, proto->isVariadic());
                    }
                    stream << ") -> ";
                    print(stream, type->getReturnType(), call_scope);
                    break;
                }

                case clang::CodeCompleteConsumer::OverloadCandidate::CK_FunctionProtoTypeLoc: {
                    auto location = candidate.getFunctionProtoTypeLoc();
                    stream << "(";
                    for(auto* param: location.getParams()) {
                        auto* identifier = param->getIdentifier();
                        auto name = identifier ? identifier->deuglifiedName() : "";
                        signature.parameter([&](llvm::raw_ostream& os) {
                            os << display::parameter(*param, name, shown_in(call_scope));
                        });
                    }
                    variadic(signature, location.getTypePtr()->isVariadic());
                    stream << ") -> ";
                    print(stream, location.getTypePtr()->getReturnType(), call_scope);
                    break;
                }

                case clang::CodeCompleteConsumer::OverloadCandidate::CK_Template: {
                    auto* declaration = candidate.getTemplate();
                    declaration->getDeclName().print(stream, policy);
                    stream << "<";
                    auto* template_parameters = declaration->getTemplateParameters();
                    for(auto* parameter: *template_parameters) {
                        signature.parameter(
                            [&](llvm::raw_ostream& os) { parameter->print(os, policy); });
                    }
                    stream << ">";
                    signature.pack =
                        template_parameters->size() > 0 &&
                        template_parameters->asArray().back()->isTemplateParameterPack();

                    const auto* scope = declaration->getDeclContext();
                    if(auto* cls = llvm::dyn_cast<clang::ClassTemplateDecl>(declaration)) {
                        stream << " -> " << cls->getTemplatedDecl()->getKindName();
                    } else if(auto* fn = llvm::dyn_cast<clang::FunctionTemplateDecl>(declaration)) {
                        stream << "() -> ";
                        print(stream, fn->getTemplatedDecl()->getReturnType(), scope);
                    } else if(auto* alias =
                                  llvm::dyn_cast<clang::TypeAliasTemplateDecl>(declaration)) {
                        stream << " -> ";
                        print(stream, alias->getTemplatedDecl()->getUnderlyingType(), scope);
                    } else if(auto* var = llvm::dyn_cast<clang::VarTemplateDecl>(declaration)) {
                        stream << " -> ";
                        print(stream, var->getTemplatedDecl()->getType(), scope);
                    } else if(llvm::isa<clang::TemplateTemplateParmDecl>(declaration)) {
                        stream << " -> type";
                    } else if(llvm::isa<clang::ConceptDecl>(declaration)) {
                        stream << " -> concept";
                    }
                    break;
                }

                case clang::CodeCompleteConsumer::OverloadCandidate::CK_Aggregate: {
                    aggregate(signature, *candidate.getAggregate());
                    break;
                }

                case clang::CodeCompleteConsumer::OverloadCandidate::CK_Function:
                case clang::CodeCompleteConsumer::OverloadCandidate::CK_FunctionTemplate:
                    std::unreachable();
            }
            add(signature, current_arg, nullptr);
        }
    }

    clang::CodeCompletionAllocator& getAllocator() final {
        return info.getAllocator();
    }

    clang::CodeCompletionTUInfo& getCodeCompletionTUInfo() final {
        return info;
    }

    /// Answers a call clang leaves unanswered, one whose callee is
    /// dependent, with the overloads the template resolver finds for it;
    /// the members of a dependent `specialization` read its template's
    /// parameters as its arguments.
    void dependent(clang::ASTContext& context,
                   llvm::ArrayRef<const clang::NamedDecl*> candidates,
                   std::uint32_t current_arg,
                   const clang::TemplateSpecializationType* specialization) {
        start(context);
        options.specialization = specialization;
        for(const auto* candidate: candidates) {
            if(const auto* function = candidate->getUnderlyingDecl()->getAsFunction()) {
                call(*function, current_arg, /*braced=*/false, /*name=*/"");
            }
        }
    }

private:
    /// Starts an answer, its types printed through `context`.
    void start(clang::ASTContext& context) {
        help.active_signature = 0;
        this->context = &context;
        readable.emplace(context, options);
        options.readable = &*readable;
        policy = display::printing_policy(context, options);
    }

    /// The options a declaration in `scope` prints its types with: named as
    /// code there names them.
    display::Options shown_in(const clang::DeclContext* scope) const {
        auto shown = options;
        shown.scope = scope;
        return shown;
    }

    /// Prints `type` as every feature shows it, named as code in `scope`
    /// names it.
    void print(llvm::raw_ostream& stream, clang::QualType type, const clang::DeclContext* scope) {
        stream << display::type(*context, type, shown_in(scope)).text;
    }

    /// A C-style `...` takes every argument past the named parameters.
    static void variadic(Signature& signature, bool variadic) {
        if(variadic) {
            signature.parameter([](llvm::raw_ostream& os) { os << "..."; }, {"..."});
            signature.pack = true;
        }
    }

    /// Adds a signature that `decl`, when given, documents.
    void add(Signature& signature, std::uint32_t argument, const clang::NamedDecl* decl) {
        sources.push_back({.decl = decl, .parameters = signature.names});
        help.signatures.push_back(std::move(signature).finish(argument));
    }

    /// Writes the parameters of `function` in [first, last).
    void parameters(Signature& signature,
                    const clang::FunctionDecl& function,
                    unsigned first,
                    unsigned last) {
        auto [pack_begin, pack_end] = expanded_pack(function);
        for(unsigned index = first; index < last; index += 1) {
            bool expanded = index >= pack_begin && index < pack_end;
            auto name = expanded ? "" : display::parameter_name(function, index);
            llvm::SmallVector<std::string, 1> names;
            if(!name.empty()) {
                names.push_back(name.str());
                for(const auto* named: decls::named_parameters(function, index)) {
                    if(!std::ranges::contains(names, named->getName())) {
                        names.push_back(named->getName().str());
                    }
                }
            }
            signature.parameter(
                [&](llvm::raw_ostream& os) {
                    os << display::parameter(*function.getParamDecl(index),
                                             name,
                                             shown_in(function.getDeclContext()));
                },
                std::move(names));
        }
    }

    /// Writes the parameters a call of `function` passes arguments to: the
    /// object parameter of a C++23 explicit object member function is left
    /// out, the object a call is made on being no argument.
    void arguments(Signature& signature, const clang::FunctionDecl& function) {
        unsigned first = function.hasCXXExplicitFunctionObjectParameter() ? 1 : 0;
        parameters(signature, function, first, function.getNumParams());
        signature.pack =
            function.getNumParams() > first && function.parameters().back()->isParameterPack();
        variadic(signature, function.isVariadic());
    }

    /// The signature of a call `candidate` answers, named `name` when the
    /// call goes through an object.
    void call(const clang::FunctionDecl& candidate,
              std::uint32_t current_arg,
              bool braced,
              llvm::StringRef name) {
        auto& function = *shown(&candidate);
        Signature signature;
        auto& stream = signature.stream;

        auto* method = llvm::dyn_cast<clang::CXXMethodDecl>(&function);
        bool lambda = method && method->getParent()->isLambda();
        if(!name.empty()) {
            stream << name;
        } else if(!lambda) {
            function.getDeclName().print(stream, policy);
        }
        stream << (braced ? "{" : "(");
        arguments(signature, function);
        stream << (braced ? "}" : ")");

        // The qualifiers of a call operator are the object's business, which a
        // call named as the object leaves out; a lambda's is const unless the
        // lambda is mutable, which is no part of calling it.
        if(method && name.empty() && !lambda) {
            stream << display::method_qualifiers(*method);
        }

        if(!llvm::isa<clang::CXXConstructorDecl, clang::CXXDestructorDecl>(function)) {
            stream << " -> ";
            print(stream, function.getReturnType(), function.getDeclContext());
        }
        add(signature, current_arg, &function);
    }

    /// Writes the bases and the fields of `record`, as aggregate
    /// initialization takes them.
    void fields(Signature& signature, const clang::RecordDecl& record) {
        if(auto* cxx_record = llvm::dyn_cast<clang::CXXRecordDecl>(&record)) {
            for(const auto& base: cxx_record->bases()) {
                signature.parameter(
                    [&](llvm::raw_ostream& os) { print(os, base.getType(), &record); });
            }
        }
        for(auto* field: record.fields()) {
            signature.parameter(
                [&](llvm::raw_ostream& os) { os << display::field(*field, shown_in(&record)); });
        }
    }

    void aggregate(Signature& signature, const clang::RecordDecl& record) {
        record.getDeclName().print(signature.stream, policy);
        signature.stream << "{";
        fields(signature, record);
        signature.stream << "}";
    }

    /// The class a standard library function forwards its trailing pack to
    /// the constructor of: `make_unique<Widget>(`, a container's
    /// `emplace_back(`. Null for any other function, and for an overload
    /// whose `initializer_list` goes to the constructor too.
    static clang::QualType forwarding_target(clang::Sema& sema,
                                             const clang::FunctionDecl& function) {
        auto* name = function.getIdentifier();
        auto params = shown(&function)->parameters();
        if(!name || !function.getTemplateSpecializationArgs() || params.empty() ||
           !params.back()->isParameterPack() ||
           std::ranges::any_of(params.drop_back(), [&](auto* param) {
               return sema.isStdInitializerList(param->getType().getNonReferenceType(), nullptr);
           })) {
            return {};
        }

        clang::QualType target;
        if(auto* method = llvm::dyn_cast<clang::CXXMethodDecl>(&function)) {
            if(!method->getParent()->isInStdNamespace() ||
               (!name->getName().starts_with("emplace") && name->getName() != "try_emplace")) {
                return {};
            }
            // A map's try_emplace builds the mapped value, every other
            // emplace an element.
            auto& member = sema.getASTContext().Idents.get(
                name->getName() == "try_emplace" ? "mapped_type" : "value_type");
            auto* alias = method->getParent()->lookup(&member).find_first<clang::TypedefNameDecl>();
            if(!alias) {
                return {};
            }
            target = alias->getUnderlyingType();
        } else {
            if(!function.isInStdNamespace() ||
               !std::ranges::contains(std::array{"make_unique", "make_shared", "allocate_shared"},
                                      name->getName())) {
                return {};
            }
            // The call names the class in its first template argument.
            target = function.getTemplateSpecializationArgs()->get(0).getAsType();
        }

        if(!target->getAsCXXRecordDecl() || !sema.isCompleteType(function.getLocation(), target)) {
            return {};
        }
        return target;
    }

    /// Answers a standard library function that forwards its arguments to
    /// a constructor with the constructors that can take the arguments
    /// written so far, each after the function's own parameters before
    /// the pack; false for any other function.
    bool forwarded(clang::Sema& sema,
                   const clang::FunctionDecl& candidate,
                   std::uint32_t current_arg,
                   clang::SourceLocation open_paren) {
        auto target = forwarding_target(sema, candidate);
        if(target.isNull()) {
            return false;
        }
        auto& function = *shown(&candidate);
        unsigned prefix = function.getNumParams() - 1;

        // The specialization Sema deduced from the arguments written so far
        // holds their types in its pack: an expression of each stands in for
        // its argument. Sema deduces from none past a dependent one, and
        // pack arguments the call writes explicitly come on top: the pack
        // then holds no type for some argument, or one for an argument not
        // written yet.
        auto template_arguments = candidate.getTemplateSpecializationArgs()->asArray();
        auto pack = std::ranges::find_if(template_arguments, [](const auto& argument) {
            return argument.getKind() == clang::TemplateArgument::Pack;
        });
        if(pack->pack_size() != (current_arg > prefix ? current_arg - prefix : 0)) {
            return false;
        }
        auto& context = sema.getASTContext();
        llvm::SmallVector<clang::Expr*> typed;
        for(const auto& element: pack->pack_elements()) {
            auto type = element.getAsType();
            typed.push_back(new (context) clang::OpaqueValueExpr(
                open_paren,
                type.getNonReferenceType(),
                type->isLValueReferenceType() ? clang::VK_LValue : clang::VK_XValue));
        }

        // A member's return type may depend on the arguments, as the
        // enable_if of optional's emplace does, so each constructor listed
        // shows the declared one; a factory's depends on the class alone.
        forwarding = Forwarding{
            .function = &function,
            .result = llvm::isa<clang::CXXMethodDecl>(function) ? function.getReturnType()
                                                                : candidate.getReturnType(),
            .argument = current_arg,
        };

        // Parentheses initialize an aggregate too since C++20, which Sema's
        // constructor help leaves to braces.
        auto* record = target->getAsCXXRecordDecl();
        if(record->isAggregate() && sema.getLangOpts().CPlusPlus20 &&
           typed.size() <
               record->getNumBases() + std::distance(record->field_begin(), record->field_end())) {
            add_forwarded(*record, [&](Signature& signature) { fields(signature, *record); });
        }
        // Sema answers the constructors through ProcessOverloadCandidates.
        sema.CodeCompletion().ProduceConstructorSignatureHelp(target,
                                                              open_paren,
                                                              typed,
                                                              open_paren,
                                                              /*Braced=*/false);
        forwarding.reset();
        return true;
    }

    /// A signature of the forwarding call being answered: its function's
    /// parameters before the pack, then what `write` writes.
    void add_forwarded(const clang::NamedDecl& documented,
                       llvm::function_ref<void(Signature&)> write) {
        auto& function = *forwarding->function;
        Signature signature;
        function.getDeclName().print(signature.stream, policy);
        signature.stream << "(";
        parameters(signature, function, 0, function.getNumParams() - 1);
        // The constructor's comment names none of the function's own.
        for(auto& names: signature.names) {
            names.clear();
        }
        write(signature);
        signature.stream << ") -> ";
        print(signature.stream, forwarding->result, function.getDeclContext());
        add(signature, forwarding->argument, &documented);
    }

    /// The forwarding call whose constructors Sema is reporting.
    struct Forwarding {
        const clang::FunctionDecl* function;
        clang::QualType result;
        /// The argument the cursor is on in the forwarding call.
        std::uint32_t argument;
    };

    std::optional<Forwarding> forwarding;

    protocol::SignatureHelp& help;

    /// What documents each signature of `help`, in its order.
    std::vector<Documented>& sources;

    clang::ASTContext* context = nullptr;
    std::optional<display::ReadableTypes> readable;

    /// Where the call Sema answers is.
    const clang::DeclContext* call_scope = nullptr;

    /// Short names, as a popup has little room: no scopes, no
    /// uglification.
    display::Options options = {
        .suppress_scope = true,
        .suppress_unwritten_scope = true,
        .suppress_ctor_template_args = true,
        .clean_uglified = true,
    };
    clang::PrintingPolicy policy{clang::LangOptions()};
    clang::CodeCompletionTUInfo info;
};

/// A call of a function-like macro whose argument list holds the cursor.
struct MacroCall {
    const clang::MacroInfo* macro;
    std::string name;

    /// The argument the cursor is on, counting from 0.
    std::uint32_t argument = 0;

    /// No parenthesis opened inside the argument list holds the cursor:
    /// it is on an argument of the macro itself, not inside a call written
    /// in one.
    bool direct = true;
};

/// The innermost call of a function-like macro around `offset`. Sema has
/// no signature for it: the parser sees only what the macro expands to.
auto macro_call_at(CompilationUnit& unit, std::uint32_t offset) -> std::optional<MacroCall> {
    auto directive = unit.directives().find(unit.focus_file().file());
    if(directive == unit.directives().end()) {
        return std::nullopt;
    }

    auto focus = unit.focus_file();
    auto content = focus.content();
    const auto& lexical = focus.lexical();

    std::optional<MacroCall> innermost;
    std::uint32_t innermost_begin = 0;
    for(const auto& ref: directive->second.macros) {
        if(ref.kind != MacroRef::Ref || !ref.macro->isFunctionLike()) {
            continue;
        }
        auto begin = unit.seen_at(ref.loc).offset;
        if(begin >= offset || (innermost && begin < innermost_begin)) {
            continue;
        }

        auto tokens = lexical.code_from(begin);
        auto token = tokens.begin();
        auto name = token->text(content);
        ++token;
        if(token == tokens.end() || token->range.begin >= offset ||
           token->kind != clang::tok::l_paren) {
            continue;
        }
        MacroCall call{.macro = ref.macro, .name = name.str()};

        std::uint32_t depth = 1;
        for(++token; token != tokens.end() && token->range.begin < offset && depth > 0; ++token) {
            if(token->kind == clang::tok::l_paren) {
                depth += 1;
            } else if(token->kind == clang::tok::r_paren) {
                depth -= 1;
            } else if(token->kind == clang::tok::comma && depth == 1) {
                call.argument += 1;
            }
        }
        if(depth == 0) {
            continue;
        }
        call.direct = depth == 1;
        innermost = std::move(call);
        innermost_begin = begin;
    }
    return innermost;
}

/// Shows `doc` with `signature` as plain text, its `@param`s under the names
/// the signature shows, and each parameter's `@param` beside it too; `names`
/// are the parameters' (Signature::names).
void document(protocol::SignatureInformation& signature,
              const Documentation& doc,
              llvm::ArrayRef<llvm::SmallVector<std::string, 1>> names) {
    if(doc.empty()) {
        return;
    }
    auto shown = doc;
    llvm::SmallVector<bool> claimed(doc.params.size());
    // Each `@param` documents one parameter: declarations may give one name
    // to two of them.
    auto claim = [&](protocol::ParameterInformation& parameter,
                     llvm::ArrayRef<std::string> written) {
        for(const auto& name: written) {
            for(auto [index, param]: std::views::enumerate(doc.params)) {
                if(!claimed[index] && param.name == name && !param.text.empty()) {
                    claimed[index] = true;
                    parameter.documentation = param.text;
                    shown.params[index].name = written.front();
                    return;
                }
            }
        }
    };
    if(signature.parameters) {
        assert(std::ranges::size(*signature.parameters) == std::ranges::size(names));
        for(auto [parameter, written]: std::views::zip(*signature.parameters, names)) {
            claim(parameter, written);
        }
    }
    signature.documentation = present(shown).as_plain_text();
}

/// A call around the cursor whose callee is dependent, which Sema leaves
/// unanswered: the callee, and the arguments before the cursor's.
struct DependentCall {
    const clang::Expr* callee;
    llvm::ArrayRef<const clang::Expr*> written;
};

/// The innermost recovery expression ending at a location.
struct RecoveryFinder : clang::RecursiveASTVisitor<RecoveryFinder> {
    explicit RecoveryFinder(clang::SourceLocation end) : end(end) {}

    clang::SourceLocation end;
    const clang::RecoveryExpr* found = nullptr;

    bool VisitRecoveryExpr(clang::RecoveryExpr* expr) {
        if(expr->getEndLoc() == end) {
            found = expr;
        }
        return true;
    }
};

/// The innermost call whose argument list holds the cursor, when its callee
/// is dependent. The parse stops at the cursor, at the start of an
/// identifier the cursor ends or is inside of, which leaves the call a
/// recovery expression of its callee and the arguments written before it,
/// ending there.
auto dependent_call_at(CompilationUnit& unit, std::uint32_t offset)
    -> std::optional<DependentCall> {
    auto focus = unit.focus_file();
    auto content = focus.content();
    auto typed = offset;
    while(typed > 0 && is_identifier_char(content[typed - 1])) {
        typed -= 1;
    }
    auto stop = focus.location(typed < offset && llvm::isDigit(content[typed]) ? offset : typed);
    RecoveryFinder finder(stop);
    for(auto* decl: unit.tu()->noload_decls()) {
        if(focus.offset(decl->getBeginLoc())) {
            finder.TraverseDecl(decl);
        }
    }
    if(!finder.found) {
        return std::nullopt;
    }
    auto parts = finder.found->subExpressions();
    if(parts.empty() || !parts.front()->isTypeDependent()) {
        return std::nullopt;
    }
    // The cursor is in the call's own argument list, right after its `(` or
    // a `,`, not in one of a call written there that clang left no node of
    // (`push(Node<T>(a, `).
    auto& sources = unit.context().getSourceManager();
    auto separator =
        clang::Lexer::findNextToken(parts.back()->getEndLoc(), sources, unit.lang_options());
    if(!separator || !separator->isOneOf(clang::tok::l_paren, clang::tok::comma)) {
        return std::nullopt;
    }
    auto next = clang::Lexer::findNextToken(separator->getLocation(), sources, unit.lang_options());
    if(next && sources.isBeforeInTranslationUnit(next->getLocation(), focus.location(typed))) {
        return std::nullopt;
    }
    return DependentCall{.callee = parts.front(), .written = parts.drop_front()};
}

auto macro_signature(const MacroCall& call, const Documentation& doc) -> protocol::SignatureHelp {
    Signature signature;
    signature.stream << call.name << "(";
    auto params = call.macro->params();
    for(auto* param: params) {
        bool last = param == params.back();
        signature.parameter(
            [&](llvm::raw_ostream& os) {
                if(last && call.macro->isC99Varargs()) {
                    os << "...";
                    return;
                }
                os << param->getName();
                if(last && call.macro->isGNUVarargs()) {
                    os << "...";
                }
            },
            {last && call.macro->isC99Varargs() ? "..." : param->getName().str()});
    }
    signature.stream << ")";
    signature.pack = call.macro->isVariadic();

    auto names = signature.names;
    protocol::SignatureHelp help;
    help.signatures.push_back(std::move(signature).finish(call.argument));
    document(help.signatures.front(), doc, names);
    help.active_signature = 0;
    return help;
}

}  // namespace

auto signature_help(CompilationParams params,
                    const SignatureHelpOptions& options,
                    const DocumentationOptions& documentation,
                    llvm::StringRef selected) -> protocol::SignatureHelp {
    protocol::SignatureHelp help;

    clang::CodeCompleteOptions complete_options;
    complete_options.IncludeMacros = false;
    complete_options.IncludeCodePatterns = false;
    complete_options.IncludeGlobals = false;
    complete_options.IncludeNamespaceLevelDecls = false;
    complete_options.IncludeBriefComments = false;
    complete_options.LoadExternal = true;
    complete_options.IncludeFixIts = false;

    auto offset = params.completion.offset;
    std::vector<Documented> sources;
    params.completion.consumer = [&](CompilationUnit&) {
        return std::make_unique<Builder>(help, sources, complete_options);
    };
    auto unit = compile(std::move(params));
    if(!unit->completed()) {
        return help;
    }

    if(help.signatures.empty()) {
        if(auto call = dependent_call_at(*unit, offset)) {
            auto& resolver = unit->get<types::TemplateResolver>();
            auto candidates = resolver.callees(call->callee, call->written);
            const auto* access = llvm::dyn_cast<clang::CXXDependentScopeMemberExpr>(call->callee);
            Builder(help, sources, complete_options)
                .dependent(unit->context(),
                           candidates,
                           static_cast<std::uint32_t>(call->written.size()),
                           access ? types::accessed_specialization(*access, resolver) : nullptr);
        }
    }

    // Inside a call written in a macro's arguments Sema's signature is the
    // call's; one written outside the macro, which Sema may report too,
    // is not where the cursor is.
    if(auto call = macro_call_at(*unit, offset);
       call && (call->direct || help.signatures.empty())) {
        return macro_signature(*call,
                               options.documentation
                                   ? feature::documentation(*unit, call->macro, documentation)
                                   : Documentation());
    }

    if(options.documentation) {
        assert(std::ranges::size(help.signatures) == std::ranges::size(sources));
        for(auto [signature, source]: std::views::zip(help.signatures, sources)) {
            if(source.decl) {
                document(signature,
                         feature::documentation(*unit, source.decl, documentation),
                         source.parameters);
            }
        }
    }

    // Overloads told apart only by what a label leaves out, such as the
    // qualifiers of a call operator named after its object, read as one.
    llvm::StringSet<> labels;
    std::erase_if(help.signatures,
                  [&](const auto& signature) { return !labels.insert(signature.label).second; });

    // The overload the client shows stays active while it is listed: the
    // user may have picked it over the one Sema ranks first.
    auto kept = std::ranges::find_if(help.signatures, [&](const auto& signature) {
        return signature.label == selected;
    });
    if(kept != help.signatures.end()) {
        help.active_signature = kept - help.signatures.begin();
    }
    return help;
}

}  // namespace clice::feature

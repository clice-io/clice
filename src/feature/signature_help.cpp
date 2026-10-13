module;

#include "modules/prelude.h"

module clice;

import :compile.compilation;
import :compile.compilation_unit;
import :compile.directive;
import :feature.feature;
import :semantic.display;
import :support.markup;
import :syntax.lexer;

namespace clice::feature {

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
    return llvm::all_of(llvm::zip(method->parameters(), better->parameters()), [&](auto pair) {
        auto [lhs, rhs] = pair;
        return context.hasSameType(lhs->getType(), rhs->getType());
    });
}

/// The declaration a candidate shows as: a function template's
/// specialization as the template declares it, a member of a class
/// template's specialization as the class instantiates it.
const clang::FunctionDecl* shown(const clang::FunctionDecl* function) {
    if(auto* primary = function->getPrimaryTemplate()) {
        return primary->getTemplatedDecl();
    }
    return function;
}

/// The declaration `function` was instantiated from. A member template of a
/// class template's specialization keeps no link of its own: its template
/// does.
const clang::FunctionDecl* pattern_of(const clang::FunctionDecl& function) {
    if(auto* pattern = function.getTemplateInstantiationPattern()) {
        return pattern;
    }
    if(auto* described = function.getDescribedFunctionTemplate()) {
        if(auto* member = described->getInstantiatedFromMemberTemplate()) {
            return member->getTemplatedDecl();
        }
    }
    return nullptr;
}

/// The name of `function`'s parameter `index`. A declaration that leaves
/// it out borrows it from another declaration of the function, or of the
/// one it was instantiated from.
llvm::StringRef parameter_name(const clang::FunctionDecl& function, unsigned index) {
    auto named = [index](const clang::FunctionDecl& declaration) -> llvm::StringRef {
        for(auto* redecl: declaration.redecls()) {
            // A C declaration without a prototype has no parameters.
            if(index >= redecl->getNumParams()) {
                continue;
            }
            if(auto* identifier = redecl->getParamDecl(index)->getIdentifier()) {
                return identifier->deuglifiedName();
            }
        }
        return {};
    };
    auto name = named(function);
    if(auto* pattern = pattern_of(function);
       name.empty() && pattern && pattern->getNumParams() == function.getNumParams()) {
        name = named(*pattern);
    }
    return name;
}

/// The parameters an instantiation expanded from a pack of the
/// declaration it was instantiated from, as indices [begin, end): each
/// is named after the pack, which names none of them. A member template
/// of a class template's specialization keeps its own pack.
std::pair<unsigned, unsigned> expanded_pack(const clang::FunctionDecl& function) {
    auto* pattern = pattern_of(function);
    if(!pattern) {
        return {};
    }
    auto params = pattern->parameters();
    auto pack = llvm::find_if(params, [](auto* param) { return param->isParameterPack(); });
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

    /// The last parameter takes every argument past the ones before it: a
    /// pack or a C-style `...`.
    bool pack = false;

    /// Writes one parameter with `print`.
    void parameter(llvm::function_ref<void(llvm::raw_ostream&)> print) {
        if(!parameters.empty()) {
            stream << ", ";
        }
        auto begin = lsp::encoded_length(label.str(), PositionEncoding::UTF16);
        print(stream);
        auto end = lsp::encoded_length(label.str(), PositionEncoding::UTF16);
        parameters.push_back({
            .label = std::tuple<protocol::uinteger, protocol::uinteger>{begin, end},
        });
    }

    /// The finished signature, the parameter `argument` falls on active.
    protocol::SignatureInformation finish(std::uint32_t argument, std::string documentation) && {
        if(pack && argument >= parameters.size()) {
            argument = static_cast<std::uint32_t>(parameters.size() - 1);
        }
        protocol::SignatureInformation signature{.label = label.str().str()};
        if(!documentation.empty()) {
            signature.documentation = std::move(documentation);
        }
        if(!parameters.empty()) {
            signature.parameters = std::move(parameters);
        }
        signature.active_parameter = protocol::nullable<protocol::uinteger>(argument);
        return signature;
    }
};

/// A function parameter as its declaration writes it, under `name`.
void print_parameter(llvm::raw_ostream& stream,
                     const clang::ParmVarDecl& param,
                     llvm::StringRef name,
                     const clang::PrintingPolicy& policy) {
    // The type as written: an array parameter does not decay. Implicit
    // special members write none.
    auto type = param.getTypeSourceInfo() ? param.getTypeSourceInfo()->getType() : param.getType();
    std::string declarator;
    if(auto* expansion = type->getAs<clang::PackExpansionType>()) {
        type = expansion->getPattern();
        declarator = "...";
    }
    declarator += name;
    type.print(stream, policy, declarator);
    if(auto value = display::default_argument(&param)) {
        stream << " = " << *value;
    }
}

class SignatureCollector final : public clang::CodeCompleteConsumer {
public:
    SignatureCollector(protocol::SignatureHelp& help,
                       clang::CodeCompleteOptions complete_options,
                       const SignatureHelpOptions& options) :
        clang::CodeCompleteConsumer(complete_options), help(help), options(options),
        info(std::make_shared<clang::GlobalCodeCompletionAllocator>()) {}

    void ProcessOverloadCandidates(clang::Sema& sema,
                                   std::uint32_t current_arg,
                                   OverloadCandidate* candidates,
                                   std::uint32_t candidate_count,
                                   clang::SourceLocation open_paren,
                                   bool braced) final {
        if(forwarding) {
            // The standard library reaches only public constructors.
            for(auto& candidate: llvm::make_range(candidates, candidates + candidate_count)) {
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
        help.active_signature = 0;

        policy = sema.getPrintingPolicy();
        policy.AnonymousTagNameStyle =
            std::to_underlying(clang::PrintingPolicy::AnonymousTagMode::Plain);
        policy.SuppressStrongLifetime = true;
        policy.SuppressUnwrittenScope = true;
        policy.SuppressScope = true;
        policy.CleanUglifiedParameters = true;
        policy.SuppressTemplateArgsInCXXConstructors = true;

        bool ranked = object_written(sema, open_paren);
        llvm::SmallVector<const clang::FunctionDecl*> listed;
        for(auto& candidate: llvm::make_range(candidates, candidates + candidate_count)) {
            if(auto* function = candidate.getFunction()) {
                if(ranked && llvm::any_of(listed, [&](auto* better) {
                       return dominated(function, better);
                   })) {
                    continue;
                }
                listed.push_back(function);
                if(!forwarded(sema, *function, current_arg, open_paren)) {
                    call(sema, *function, current_arg, open_paren, braced);
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
                                [&](llvm::raw_ostream& os) { param_type.print(os, policy); });
                        }
                        variadic(signature, proto->isVariadic());
                    }
                    stream << ") -> ";
                    type->getReturnType().print(stream, policy);
                    break;
                }

                case clang::CodeCompleteConsumer::OverloadCandidate::CK_FunctionProtoTypeLoc: {
                    auto location = candidate.getFunctionProtoTypeLoc();
                    stream << "(";
                    for(auto* param: location.getParams()) {
                        auto* identifier = param->getIdentifier();
                        auto name = identifier ? identifier->deuglifiedName() : "";
                        signature.parameter([&](llvm::raw_ostream& os) {
                            print_parameter(os, *param, name, policy);
                        });
                    }
                    variadic(signature, location.getTypePtr()->isVariadic());
                    stream << ") -> ";
                    location.getTypePtr()->getReturnType().print(stream, policy);
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

                    if(auto* cls = llvm::dyn_cast<clang::ClassTemplateDecl>(declaration)) {
                        stream << " -> " << cls->getTemplatedDecl()->getKindName();
                    } else if(auto* fn = llvm::dyn_cast<clang::FunctionTemplateDecl>(declaration)) {
                        stream << "() -> ";
                        fn->getTemplatedDecl()->getReturnType().print(stream, policy);
                    } else if(auto* alias =
                                  llvm::dyn_cast<clang::TypeAliasTemplateDecl>(declaration)) {
                        stream << " -> ";
                        alias->getTemplatedDecl()->getUnderlyingType().print(stream, policy);
                    } else if(auto* var = llvm::dyn_cast<clang::VarTemplateDecl>(declaration)) {
                        stream << " -> ";
                        var->getTemplatedDecl()->getType().print(stream, policy);
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
            help.signatures.push_back(std::move(signature).finish(current_arg, {}));
        }
    }

    clang::CodeCompletionAllocator& getAllocator() final {
        return info.getAllocator();
    }

    clang::CodeCompletionTUInfo& getCodeCompletionTUInfo() final {
        return info;
    }

private:
    /// A C-style `...` takes every argument past the named parameters.
    static void variadic(Signature& signature, bool variadic) {
        if(variadic) {
            signature.parameter([](llvm::raw_ostream& os) { os << "..."; });
            signature.pack = true;
        }
    }

    /// The documentation comment of `decl`, as plain text.
    std::string documentation(const clang::NamedDecl& decl) {
        if(!options.documentation) {
            return {};
        }
        auto comment = display::comment(&decl);
        if(comment.empty()) {
            return {};
        }
        markup::Document document;
        parse_documentation(comment, document);
        return document.as_plain_text();
    }

    /// Writes the parameters of `function` in [first, last).
    void parameters(Signature& signature,
                    const clang::FunctionDecl& function,
                    unsigned first,
                    unsigned last) {
        auto [pack_begin, pack_end] = expanded_pack(function);
        for(unsigned index = first; index < last; index += 1) {
            bool expanded = index >= pack_begin && index < pack_end;
            auto name = expanded ? "" : parameter_name(function, index);
            signature.parameter([&](llvm::raw_ostream& os) {
                print_parameter(os, *function.getParamDecl(index), name, policy);
            });
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

    /// The signature of a call `candidate` answers.
    void call(clang::Sema& sema,
              const clang::FunctionDecl& candidate,
              std::uint32_t current_arg,
              clang::SourceLocation open_paren,
              bool braced) {
        auto& function = *shown(&candidate);
        Signature signature;
        auto& stream = signature.stream;

        auto* method = llvm::dyn_cast<clang::CXXMethodDecl>(&function);
        bool lambda = method && method->getParent()->isLambda();
        // A call through an object reads as a call of a function named as
        // the object.
        std::string name;
        if(method && method->getOverloadedOperator() == clang::OO_Call) {
            name = callee_name(sema, open_paren);
        }
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
            if(auto qualifiers = method->getMethodQualifiers(); !qualifiers.empty()) {
                stream << " " << qualifiers.getAsString(policy);
            }
            if(method->getRefQualifier() == clang::RQ_LValue) {
                stream << " &";
            } else if(method->getRefQualifier() == clang::RQ_RValue) {
                stream << " &&";
            }
        }

        if(!llvm::isa<clang::CXXConstructorDecl, clang::CXXDestructorDecl>(function)) {
            stream << " -> ";
            function.getReturnType().print(stream, policy);
        }
        help.signatures.push_back(
            std::move(signature).finish(current_arg, documentation(function)));
    }

    /// Writes the bases and the fields of `record`, as aggregate
    /// initialization takes them.
    void fields(Signature& signature, const clang::RecordDecl& record) {
        if(auto* cxx_record = llvm::dyn_cast<clang::CXXRecordDecl>(&record)) {
            for(const auto& base: cxx_record->bases()) {
                signature.parameter(
                    [&](llvm::raw_ostream& os) { base.getType().print(os, policy); });
            }
        }
        for(auto* field: record.fields()) {
            signature.parameter([&](llvm::raw_ostream& os) { field->print(os, policy); });
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
           !params.back()->isParameterPack() || llvm::any_of(params.drop_back(), [&](auto* param) {
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
               !llvm::is_contained({"make_unique", "make_shared", "allocate_shared"},
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
        auto pack = llvm::find_if(template_arguments, [](const auto& argument) {
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

        // The standard library writes make_unique's return type through
        // its own aliases; a member's is a member type of the container.
        auto result = llvm::isa<clang::CXXMethodDecl>(function)
                          ? function.getReturnType()
                          : candidate.getReturnType().getCanonicalType();
        forwarding = Forwarding{
            .function = &function,
            .result = result,
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
        write(signature);
        signature.stream << ") -> ";
        forwarding->result.print(signature.stream, policy);
        help.signatures.push_back(
            std::move(signature).finish(forwarding->argument, documentation(documented)));
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
    const SignatureHelpOptions& options;
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
auto macro_call_at(CompilationUnitRef unit, std::uint32_t offset) -> std::optional<MacroCall> {
    auto directive = unit.directives().find(unit.main_file());
    if(directive == unit.directives().end()) {
        return std::nullopt;
    }

    // Clang's copy of the buffer carries a NUL at the completion point:
    // the text before it is the text as the user has it.
    auto text = unit.main_content().take_front(offset).str();

    std::optional<MacroCall> innermost;
    std::uint32_t innermost_begin = 0;
    for(const auto& ref: directive->second.macros) {
        if(ref.kind != MacroRef::Ref || !ref.macro->isFunctionLike() || !ref.loc.isFileID()) {
            continue;
        }
        auto begin = unit.file_offset(ref.loc);
        if(begin >= offset || (innermost && begin < innermost_begin)) {
            continue;
        }

        auto lexer = Lexer::from_line(text, begin, {.lang_opts = &unit.lang_options()});
        auto token = lexer.advance();
        while(!token.is_eof() && token.range.begin < begin) {
            token = lexer.advance();
        }
        auto name = token.text(text);
        if(lexer.advance().kind != clang::tok::l_paren) {
            continue;
        }
        MacroCall call{.macro = ref.macro, .name = name.str()};

        std::uint32_t depth = 1;
        for(token = lexer.advance(); !token.is_eof() && depth > 0; token = lexer.advance()) {
            if(token.kind == clang::tok::l_paren) {
                depth += 1;
            } else if(token.kind == clang::tok::r_paren) {
                depth -= 1;
            } else if(token.kind == clang::tok::comma && depth == 1) {
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

auto macro_signature(const MacroCall& call) -> protocol::SignatureHelp {
    Signature signature;
    signature.stream << call.name << "(";
    auto params = call.macro->params();
    for(auto* param: params) {
        bool last = param == params.back();
        signature.parameter([&](llvm::raw_ostream& os) {
            if(last && call.macro->isC99Varargs()) {
                os << "...";
                return;
            }
            os << param->getName();
            if(last && call.macro->isGNUVarargs()) {
                os << "...";
            }
        });
    }
    signature.stream << ")";
    signature.pack = call.macro->isVariadic();

    protocol::SignatureHelp help;
    help.signatures.push_back(std::move(signature).finish(call.argument, {}));
    help.active_signature = 0;
    return help;
}

}  // namespace

auto signature_help(CompilationParams& params,
                    const SignatureHelpOptions& options,
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

    auto offset = std::get<1>(params.completion);
    auto* consumer = new SignatureCollector(help, complete_options, options);
    auto unit = complete(params, consumer);
    if(!unit.completed()) {
        return help;
    }

    // Inside a call written in a macro's arguments Sema's signature is the
    // call's; one written outside the macro, which Sema may report too,
    // is not where the cursor is.
    if(auto call = macro_call_at(unit, offset); call && (call->direct || help.signatures.empty())) {
        return macro_signature(*call);
    }

    // Overloads told apart only by what a label leaves out, such as the
    // qualifiers of a call operator named after its object, read as one.
    llvm::StringSet<> labels;
    llvm::erase_if(help.signatures,
                   [&](const auto& signature) { return !labels.insert(signature.label).second; });

    // The overload the client shows stays active while it is listed: the
    // user may have picked it over the one Sema ranks first.
    auto kept = llvm::find_if(help.signatures,
                              [&](const auto& signature) { return signature.label == selected; });
    if(kept != help.signatures.end()) {
        help.active_signature = kept - help.signatures.begin();
    }
    return help;
}

}  // namespace clice::feature

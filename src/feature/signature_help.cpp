module;

#include "modules/prelude.h"

module clice;

import :compile.compilation;
import :compile.compilation_unit;
import :compile.directive;
import :feature.feature;
import :syntax.lexer;

namespace clice::feature {

namespace {

/// The name a call of a lambda goes by: the identifier written before its
/// opening parenthesis (`square` in `square(`), empty when another
/// expression is called (`make()(`).
std::string callee_name(clang::Sema& sema, clang::SourceLocation open_paren) {
    if(!open_paren.isFileID()) {
        return {};
    }
    auto token = clang::Lexer::findPreviousToken(open_paren,
                                                 sema.getSourceManager(),
                                                 sema.getLangOpts(),
                                                 /*IncludeComments=*/false);
    if(!token || !token->is(clang::tok::raw_identifier)) {
        return {};
    }
    return token->getRawIdentifier().str();
}

/// Whether `method` differs from `better`, a method Sema ranked above it,
/// only in its qualifiers: with the same parameters it is never the one a
/// call picks, as the const overload on a non-const object.
bool dominated(const clang::FunctionDecl* method, const clang::FunctionDecl* better) {
    if(!llvm::isa<clang::CXXMethodDecl>(method) || !llvm::isa<clang::CXXMethodDecl>(better) ||
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

/// Gives `signature` its parameters, the one `argument` falls on active:
/// past the last one, a trailing pack takes every further argument.
void set_parameters(protocol::SignatureInformation& signature,
                    std::vector<protocol::ParameterInformation> parameters,
                    std::uint32_t argument,
                    bool pack) {
    if(pack && argument >= parameters.size()) {
        argument = static_cast<std::uint32_t>(parameters.size() - 1);
    }
    signature.active_parameter = protocol::nullable<protocol::uinteger>(argument);
    if(!parameters.empty()) {
        signature.parameters = std::move(parameters);
    }
}

class SignatureCollector final : public clang::CodeCompleteConsumer {
public:
    SignatureCollector(protocol::SignatureHelp& help, clang::CodeCompleteOptions complete_options) :
        clang::CodeCompleteConsumer(complete_options), help(help),
        info(std::make_shared<clang::GlobalCodeCompletionAllocator>()) {}

    void ProcessOverloadCandidates(clang::Sema& sema,
                                   std::uint32_t current_arg,
                                   OverloadCandidate* candidates,
                                   std::uint32_t candidate_count,
                                   clang::SourceLocation open_paren,
                                   bool) final {
        help.signatures.reserve(candidate_count);
        help.active_signature = 0;

        auto range = llvm::make_range(candidates, candidates + candidate_count);

        auto policy = sema.getPrintingPolicy();
        policy.AnonymousTagNameStyle =
            std::to_underlying(clang::PrintingPolicy::AnonymousTagMode::Plain);
        policy.SuppressStrongLifetime = true;
        policy.SuppressUnwrittenScope = true;
        policy.SuppressScope = true;
        policy.CleanUglifiedParameters = true;
        policy.SuppressTemplateArgsInCXXConstructors = true;

        llvm::SmallVector<const clang::FunctionDecl*> listed;
        for(auto& candidate: range) {
            if(auto* function = candidate.getFunction()) {
                if(llvm::any_of(listed,
                                [&](auto* better) { return dominated(function, better); })) {
                    continue;
                }
                listed.push_back(function);
                if(auto* pattern = function->getTemplateInstantiationPattern()) {
                    candidate = OverloadCandidate(pattern);
                }
            }

            llvm::SmallString<128> buffer;
            llvm::raw_svector_ostream stream(buffer);

            auto& signature = help.signatures.emplace_back();
            std::vector<protocol::ParameterInformation> parameters;
            /// The last parameter is a pack, which takes every argument
            /// past the ones before it.
            bool pack = false;

            auto add_parameter = [&](auto&& param) {
                if(!parameters.empty()) {
                    stream << ", ";
                }

                protocol::ParameterInformation parameter;
                auto begin = lsp::encoded_length(buffer.str(), PositionEncoding::UTF16);
                param.print(stream, policy);
                auto end = lsp::encoded_length(buffer.str(), PositionEncoding::UTF16);
                parameter.label = std::tuple<protocol::uinteger, protocol::uinteger>{begin, end};
                parameters.push_back(std::move(parameter));
            };

            switch(candidate.getKind()) {
                case clang::CodeCompleteConsumer::OverloadCandidate::CK_Function:
                case clang::CodeCompleteConsumer::OverloadCandidate::CK_FunctionTemplate: {
                    auto* function = candidate.getFunction();
                    auto* method = llvm::dyn_cast<clang::CXXMethodDecl>(function);
                    bool lambda = method && method->getParent()->isLambda();
                    if(lambda) {
                        stream << callee_name(sema, open_paren);
                    } else {
                        function->getDeclName().print(stream, policy);
                    }
                    stream << "(";
                    for(auto* parameter: function->parameters()) {
                        add_parameter(*parameter);
                    }
                    stream << ")";
                    pack = !function->parameters().empty() &&
                           function->parameters().back()->isParameterPack();

                    // A lambda's call operator is const unless the lambda
                    // is mutable, which is no part of calling it.
                    if(method && !lambda) {
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
                        function->getReturnType().print(stream, policy);
                    }
                    break;
                }

                case clang::CodeCompleteConsumer::OverloadCandidate::CK_FunctionType: {
                    auto type = candidate.getFunctionType();
                    stream << "(";
                    if(auto* proto = llvm::dyn_cast<clang::FunctionProtoType>(type)) {
                        for(auto param_type: proto->param_types()) {
                            add_parameter(param_type);
                        }
                    }
                    stream << ") -> ";
                    type->getReturnType().print(stream, policy);
                    break;
                }

                case clang::CodeCompleteConsumer::OverloadCandidate::CK_FunctionProtoTypeLoc: {
                    auto location = candidate.getFunctionProtoTypeLoc();
                    stream << "(";
                    for(auto param: location.getParams()) {
                        add_parameter(*param);
                    }
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
                        add_parameter(*parameter);
                    }
                    stream << ">";
                    pack = template_parameters->size() > 0 &&
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
                    auto* cls = candidate.getAggregate();
                    cls->getDeclName().print(stream, policy);
                    stream << "{";

                    if(auto* record = llvm::dyn_cast<clang::CXXRecordDecl>(cls)) {
                        for(const auto& base: record->bases()) {
                            add_parameter(base.getType());
                        }
                    }

                    for(auto* field: cls->fields()) {
                        add_parameter(*field);
                    }
                    stream << "}";
                    break;
                }
            }

            signature.label = buffer.str().str();
            set_parameters(signature, std::move(parameters), current_arg, pack);
        }
    }

    clang::CodeCompletionAllocator& getAllocator() final {
        return info.getAllocator();
    }

    clang::CodeCompletionTUInfo& getCodeCompletionTUInfo() final {
        return info;
    }

private:
    protocol::SignatureHelp& help;
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

        auto lexer = Lexer::from_line(text, begin);
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
    std::string label = call.name + "(";
    std::vector<protocol::ParameterInformation> parameters;
    auto params = call.macro->params();
    for(auto* param: params) {
        if(!parameters.empty()) {
            label += ", ";
        }
        auto begin = lsp::encoded_length(label, PositionEncoding::UTF16);
        bool last = param == params.back();
        if(last && call.macro->isC99Varargs()) {
            label += "...";
        } else {
            label += param->getName();
            if(last && call.macro->isGNUVarargs()) {
                label += "...";
            }
        }
        auto end = lsp::encoded_length(label, PositionEncoding::UTF16);
        parameters.push_back({
            .label = std::tuple<protocol::uinteger, protocol::uinteger>{begin, end},
        });
    }
    label += ")";

    protocol::SignatureHelp help;
    auto& signature = help.signatures.emplace_back();
    signature.label = std::move(label);
    set_parameters(signature, std::move(parameters), call.argument, call.macro->isVariadic());
    help.active_signature = 0;
    return help;
}

}  // namespace

auto signature_help(CompilationParams& params, const SignatureHelpOptions&)
    -> protocol::SignatureHelp {
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
    auto* consumer = new SignatureCollector(help, complete_options);
    auto unit = complete(params, consumer);

    // Inside a call written in a macro's arguments Sema's signature is the
    // call's; one written outside the macro, which Sema may report too,
    // is not where the cursor is.
    if(auto call = macro_call_at(unit, offset); call && (call->direct || help.signatures.empty())) {
        return macro_signature(*call);
    }
    return help;
}

}  // namespace clice::feature

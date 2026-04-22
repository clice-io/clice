#include "feature/feature.h"

#include "clang/Sema/CodeCompleteConsumer.h"
#include "clang/Sema/Sema.h"

namespace clice::feature {

namespace {

class SignatureCollector final : public clang::CodeCompleteConsumer {
public:
    SignatureCollector(protocol::SignatureHelp& help, clang::CodeCompleteOptions complete_options) :
        clang::CodeCompleteConsumer(complete_options), help(help),
        info(std::make_shared<clang::GlobalCodeCompletionAllocator>()) {}

    void ProcessOverloadCandidates(clang::Sema& sema,
                                   std::uint32_t current_arg,
                                   OverloadCandidate* candidates,
                                   std::uint32_t candidate_count,
                                   clang::SourceLocation,
                                   bool) final {
        help.signatures.reserve(candidate_count);
        help.active_signature = 0;

        auto range = llvm::make_range(candidates, candidates + candidate_count);

        auto policy = sema.getPrintingPolicy();
        policy.AnonymousTagNameStyle =
            llvm::to_underlying(clang::PrintingPolicy::AnonymousTagMode::Plain);
        policy.SuppressStrongLifetime = true;
        policy.SuppressUnwrittenScope = true;
        policy.SuppressScope = true;
        policy.CleanUglifiedParameters = true;
        policy.SuppressTemplateArgsInCXXConstructors = true;

        for(auto& candidate: range) {
            if(auto* function = candidate.getFunction()) {
                if(auto* pattern = function->getTemplateInstantiationPattern()) {
                    candidate = OverloadCandidate(pattern);
                }
            }

            llvm::SmallString<128> buffer;
            llvm::raw_svector_ostream stream(buffer);

            auto& signature = help.signatures.emplace_back();
            signature.active_parameter = protocol::nullable<protocol::uinteger>(current_arg);

            auto add_parameter = [&](auto&& param) {
                if(!signature.parameters.has_value()) {
                    signature.parameters = std::vector<protocol::ParameterInformation>();
                }

                if(!signature.parameters->empty()) {
                    stream << ", ";
                }

                protocol::ParameterInformation parameter;
                auto begin = static_cast<protocol::uinteger>(buffer.size());
                param.print(stream, policy);
                auto end = static_cast<protocol::uinteger>(buffer.size());
                parameter.label = std::tuple<protocol::uinteger, protocol::uinteger>{begin, end};
                signature.parameters->push_back(std::move(parameter));
            };

            switch(candidate.getKind()) {
                case clang::CodeCompleteConsumer::OverloadCandidate::CK_Function:
                case clang::CodeCompleteConsumer::OverloadCandidate::CK_FunctionTemplate: {
                    auto* function = candidate.getFunction();
                    function->getDeclName().print(stream, policy);
                    stream << "(";
                    for(auto* parameter: function->parameters()) {
                        add_parameter(*parameter);
                    }
                    stream << ")";

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
                    for(auto* parameter: *declaration->getTemplateParameters()) {
                        add_parameter(*parameter);
                    }
                    stream << ">";

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

    auto* consumer = new SignatureCollector(help, complete_options);
    auto unit = complete(params, consumer);
    (void)unit;

    return help;
}

}  // namespace clice::feature

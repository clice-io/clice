#include "Feature/SignatureHelp.h"

#include "Compiler/Compilation.h"

#include "clang/Sema/CodeCompleteConsumer.h"
#include "clang/Sema/Sema.h"

namespace clice::feature {

namespace {

class Collector final : public clang::CodeCompleteConsumer {
public:
    Collector(proto::SignatureHelp& help, clang::CodeCompleteOptions complete_options) :
        clang::CodeCompleteConsumer(complete_options), help(help),
        info(std::make_shared<clang::GlobalCodeCompletionAllocator>()) {}

    void ProcessOverloadCandidates(clang::Sema& sema,
                                   std::uint32_t current_arg,
                                   OverloadCandidate* candidates,
                                   std::uint32_t candidate_count,
                                   clang::SourceLocation open_paren_loc,
                                   bool braced) final {
        help.signatures.reserve(candidate_count);

        // FIXME: How can we determine the "active overload candidate"?
        // Right now the overloaded candidates seem to be provided in a "best fit"
        // order, so I'm not too worried about this.
        help.activeSignature = 0;

        auto range = llvm::make_range(candidates, candidates + candidate_count);

        auto policy = sema.getPrintingPolicy();
        policy.AnonymousTagLocations = false;
        policy.SuppressStrongLifetime = true;
        policy.SuppressUnwrittenScope = true;
        policy.SuppressScope = true;
        policy.CleanUglifiedParameters = true;
        // Show signatures of constructors as they are declared:
        //   vector(int n) rather than vector<string>(int n)
        // This is less noisy without being less clear, and avoids tricky cases.
        policy.SuppressTemplateArgsInCXXConstructors = true;

        for(auto& candidate: range) {
            /// We want to avoid showing instantiated signatures, because they may be
            /// long in some cases (e.g. when 'T' is substituted with 'std::string', we
            /// would get 'std::basic_string<char>').
            /// FIXME: In fact, in such case, we may resugar the template arguments.
            if(auto func = candidate.getFunction()) {
                if(auto pattern = func->getTemplateInstantiationPattern()) {
                    candidate = OverloadCandidate(pattern);
                }
            }

            llvm::SmallString<128> buffer;
            llvm::raw_svector_ostream os(buffer);

            auto& signature = help.signatures.emplace_back();

            /// FIXME: Handle explicit this and variadic params...
            signature.activeParameter = current_arg;

            auto add_param = [&](auto&& param) {
                if(signature.parameters.size() > 0) {
                    os << ", ";
                }

                /// FIXME: Handle param comments in the future.
                auto& label = signature.parameters.emplace_back().label;
                label[0] = buffer.size();
                param.print(os, policy);
                label[1] = buffer.size();
            };

            switch(candidate.getKind()) {
                case clang::CodeCompleteConsumer::OverloadCandidate::CK_Function:
                case clang::CodeCompleteConsumer::OverloadCandidate::CK_FunctionTemplate: {
                    auto func = candidate.getFunction();
                    func->getDeclName().print(os, policy);
                    os << "(";
                    /// FIXME: Handle C++23 explicit object params.
                    for(auto param: func->parameters()) {
                        add_param(*param);
                    }
                    os << ")";

                    if(!llvm::isa<clang::CXXConstructorDecl, clang::CXXDestructorDecl>(func)) {
                        os << " -> ";
                        func->getReturnType().print(os, policy);
                    }

                    break;
                }

                case clang::CodeCompleteConsumer::OverloadCandidate::CK_FunctionType: {
                    auto type = candidate.getFunctionType();
                    os << "(";
                    if(auto proto = llvm::dyn_cast<clang::FunctionProtoType>(type)) {
                        for(auto type: proto->param_types()) {
                            add_param(type);
                        }
                    }
                    os << ") -> ";
                    type->getReturnType().print(os, policy);
                    break;
                }

                case clang::CodeCompleteConsumer::OverloadCandidate::CK_FunctionProtoTypeLoc: {
                    auto loc = candidate.getFunctionProtoTypeLoc();
                    os << "(";
                    for(auto type: loc.getParams()) {
                        add_param(*type);
                    }
                    os << ") -> ";
                    loc.getTypePtr()->getReturnType().print(os, policy);
                    break;
                }

                case clang::CodeCompleteConsumer::OverloadCandidate::CK_Template: {
                    auto decl = candidate.getTemplate();
                    /// Add template name first.
                    decl->getDeclName().print(os, policy);
                    os << "<";
                    for(auto param: *decl->getTemplateParameters()) {
                        add_param(*param);
                    }
                    os << "> ";

                    if(auto cls = llvm::dyn_cast<clang::ClassTemplateDecl>(decl)) {
                        os << "-> ";
                        os << cls->getTemplatedDecl()->getKindName();
                    } else if(auto func = llvm::dyn_cast<clang::FunctionTemplateDecl>(decl)) {
                        os << "() -> ";
                        func->getTemplatedDecl()->getReturnType().print(os, policy);
                    } else if(auto type = llvm::dyn_cast<clang::TypeAliasTemplateDecl>(decl)) {
                        os << "-> ";
                        type->getTemplatedDecl()->getUnderlyingType().print(os, policy);
                    } else if(auto var = llvm::dyn_cast<clang::VarTemplateDecl>(decl)) {
                        os << "-> ";
                        var->getTemplatedDecl()->getType().print(os, policy);
                    } else if(auto tmp = llvm::dyn_cast<clang::TemplateTemplateParmDecl>(decl)) {
                        os << "-> type";
                    } else if(auto con = llvm::dyn_cast<clang::ConceptDecl>(decl)) {
                        os << "-> concept";
                    } else {
                        std::unreachable();
                    }

                    break;
                }

                case clang::CodeCompleteConsumer::OverloadCandidate::CK_Aggregate: {
                    auto cls = candidate.getAggregate();
                    cls->getDeclName().print(os, policy);
                    os << "{";

                    if(auto type = llvm::dyn_cast<clang::CXXRecordDecl>(cls)) {
                        for(auto& base: type->bases()) {
                            add_param(base.getType());
                        }
                    }

                    for(auto field: cls->fields()) {
                        add_param(*field);
                    }
                    os << "}";
                    break;
                }
            }

            signature.label = buffer.str();
        }

        /// FIXME: Sort the result according the params num and kind ...
    }

    clang::CodeCompletionAllocator& getAllocator() final {
        return info.getAllocator();
    }

    clang::CodeCompletionTUInfo& getCodeCompletionTUInfo() final {
        return info;
    }

private:
    proto::SignatureHelp& help;
    clang::CodeCompletionTUInfo info;
};

}  // namespace

proto::SignatureHelp signature_help(CompilationParams& params,
                                    const config::SignatureHelpOption& options) {
    proto::SignatureHelp help;

    clang::CodeCompleteOptions complete_options;
    complete_options.IncludeMacros = false;
    complete_options.IncludeCodePatterns = false;
    complete_options.IncludeGlobals = false;
    complete_options.IncludeNamespaceLevelDecls = false;
    complete_options.IncludeBriefComments = false;
    complete_options.LoadExternal = true;
    complete_options.IncludeFixIts = false;

    auto consumer = new Collector(help, complete_options);
    if(auto info = complete(params, consumer)) {
        /// FIXME: do something.
    }
    return help;
}

}  // namespace clice::feature

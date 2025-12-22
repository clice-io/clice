#include "Feature/CodeCompletion.h"

#include "AST/SymbolKind.h"
#include "AST/Utility.h"
#include "Compiler/Compilation.h"
#include "Support/FuzzyMatcher.h"

#include "clang/Basic/CharInfo.h"
#include "clang/Lex/Preprocessor.h"
#include "clang/Sema/CodeCompleteConsumer.h"
#include "clang/Sema/Sema.h"

namespace clice::feature {

namespace {

struct CompletionPrefix {
    /// The range that will replaced.
    LocalSourceRange range;

    /// The unqualified partial name.
    /// If there is none, begin() == end() == completion position.
    llvm::StringRef spelling;

    /// The spelled scope qualifier, such as Foo::.
    /// If there is none, begin() == end() == name.begin().
    /// FIXME: llvm::StringRef qualifier;

    static CompletionPrefix from(llvm::StringRef content, std::uint32_t offset) {
        assert(offset <= content.size());

        auto start = offset;
        while(start > 0 && clang::isAsciiIdentifierContinue(content[start - 1])) {
            --start;
        }

        auto end = offset;
        while(end < content.size() && clang::isAsciiIdentifierContinue(content[end])) {
            ++end;
        }

        return CompletionPrefix{
            LocalSourceRange{start, end},
            content.substr(start, offset - start),
        };
    }
};

/// Get the code completion kind of named declaration.
CompletionItemKind completion_kind(const clang::NamedDecl* decl) {
    switch(decl->getKind()) {
        case clang::Decl::Namespace:
        case clang::Decl::NamespaceAlias: {
            /// TODO: Extend `namespace` kind.
            return CompletionItemKind::Module;
        }

        case clang::Decl::Function:
        case clang::Decl::FunctionTemplate: {
            return CompletionItemKind::Function;
        }

        case clang::Decl::CXXMethod:
        case clang::Decl::CXXConversion:
        case clang::Decl::CXXDestructor:
        case clang::Decl::CXXDeductionGuide: {
            return CompletionItemKind::Method;
        }

        case clang::Decl::CXXConstructor: {
            return CompletionItemKind::Constructor;
        }

        case clang::Decl::Var:
        case clang::Decl::ParmVar:
        case clang::Decl::ImplicitParam:
        case clang::Decl::Binding:
        case clang::Decl::Decomposition:
        case clang::Decl::VarTemplate:
        case clang::Decl::VarTemplateSpecialization:
        case clang::Decl::VarTemplatePartialSpecialization:
        case clang::Decl::NonTypeTemplateParm: {
            return CompletionItemKind::Variable;
        }

        case clang::Decl::Label: {
            return CompletionItemKind::Variable;
        }

        case clang::Decl::Enum: {
            return CompletionItemKind::Enum;
        }

        case clang::Decl::EnumConstant: {
            return CompletionItemKind::EnumMember;
        }

        case clang::Decl::Field:
        case clang::Decl::IndirectField: {
            return CompletionItemKind::Field;
        }

        case clang::Decl::Record:
        case clang::Decl::CXXRecord:
        case clang::Decl::ClassTemplate:
        case clang::Decl::ClassTemplateSpecialization:
        case clang::Decl::ClassTemplatePartialSpecialization: {
            return CompletionItemKind::Class;
        }

        case clang::Decl::Typedef:
        case clang::Decl::TypeAlias:
        case clang::Decl::TemplateTypeParm:
        case clang::Decl::TemplateTemplateParm:
        case clang::Decl::TypeAliasTemplate:
        case clang::Decl::Concept:
        case clang::Decl::BuiltinTemplate: {
            return CompletionItemKind::TypeParameter;
        }

        case clang::Decl::UnresolvedUsingValue:
        case clang::Decl::UnnamedGlobalConstant:
        case clang::Decl::TemplateParamObject: {
            return CompletionItemKind::Constant;
        }

        case clang::Decl::MSGuid:
        case clang::Decl::MSProperty:
        case clang::Decl::Using:
        case clang::Decl::UsingPack:
        case clang::Decl::UsingEnum:
        case clang::Decl::UsingShadow:
        case clang::Decl::UsingDirective:
        case clang::Decl::UnresolvedUsingIfExists:
        case clang::Decl::UnresolvedUsingTypename:
        case clang::Decl::ConstructorUsingShadow: {
            /// FIXME: Is it possible that above kinds occur in
            /// code completion result?
            llvm_unreachable("Unexpected declaration");
        }

        /// The following kinds are not `NamedDecl`, we just ignore them.
        case clang::Decl::TranslationUnit:
        case clang::Decl::PragmaComment:
        case clang::Decl::PragmaDetectMismatch:
        case clang::Decl::ExternCContext:
        case clang::Decl::ImplicitConceptSpecialization:
        case clang::Decl::LinkageSpec:
        case clang::Decl::Export:
        case clang::Decl::FileScopeAsm:
        case clang::Decl::TopLevelStmt:
        case clang::Decl::AccessSpec:
        case clang::Decl::Friend:
        case clang::Decl::FriendTemplate:
        case clang::Decl::StaticAssert:
        case clang::Decl::Block:
        /// case clang::Decl::OutlinedFunction:
        case clang::Decl::Captured:
        case clang::Decl::Import:
        case clang::Decl::Empty:
        case clang::Decl::RequiresExprBody:
        case clang::Decl::LifetimeExtendedTemporary:

        case clang::Decl::OMPAllocate:
        case clang::Decl::OMPRequires:
        case clang::Decl::OMPDeclareMapper:
        case clang::Decl::OMPCapturedExpr:
        case clang::Decl::OMPThreadPrivate:
        case clang::Decl::OMPDeclareReduction:

        case clang::Decl::ObjCIvar:
        case clang::Decl::ObjCMethod:
        case clang::Decl::ObjCProtocol:
        case clang::Decl::ObjCProperty:
        case clang::Decl::ObjCCategory:
        case clang::Decl::ObjCInterface:
        case clang::Decl::ObjCTypeParam:
        case clang::Decl::ObjCAtDefsField:
        case clang::Decl::ObjCPropertyImpl:
        case clang::Decl::ObjCCategoryImpl:
        case clang::Decl::ObjCImplementation:
        case clang::Decl::ObjCCompatibleAlias:

        case clang::Decl::OutlinedFunction:
        case clang::Decl::HLSLBuffer:
        case clang::Decl::OpenACCRoutine:
        case clang::Decl::OpenACCDeclare:
        case clang::Decl::HLSLRootSignature: {
            std::unreachable();
        }
    }
}

/// Calculate the final code completion result.
class CompletionRender {
public:
    CompletionRender(clang::Sema& sema,
                     std::uint32_t offset,
                     llvm::StringRef content,
                     std::vector<CompletionItem>& items,
                     clang::CodeCompletionContext& cc_context,
                     const config::CodeCompletionOption& option) :
        sema(sema), context(sema.getASTContext()), content(content),
        prefix(CompletionPrefix::from(content, offset)), items(items), matcher(prefix.spelling),
        cc_context(cc_context), option(option) {}

    struct OverloadSet {
        /// The first declaration of this overload set.
        const clang::NamedDecl* first;

        /// The score of the overload name.
        float score;

        /// The count of functions in this overload set.
        std::uint32_t count;
    };

    /// Render edit text for declaration.
    std::string render(const clang::NamedDecl* decl) {
        return ast::name_of(decl);
    }

    void process_candidate(clang::CodeCompletionResult& candidate) {
        feature::CompletionItem item;

        item.edit.range = prefix.range;

        /// Check whether the name matchs, if so, set the item.
        auto check_name = [&](llvm::StringRef name) {
            auto score = matcher.match(name);
            if(!score) {
                return false;
            }

            item.label = name;
            item.score = *score;
            item.edit.text = item.label;
            return true;
        };

        switch(candidate.Kind) {
            case clang::CodeCompletionResult::RK_Keyword: {
                if(!check_name(candidate.Keyword)) {
                    return;
                }

                item.edit.text = item.label;
                item.kind = feature::CompletionItemKind::Keyword;
                item.edit.text = item.label;
                break;
            }

            case clang::CodeCompletionResult::RK_Pattern: {
                auto text = candidate.Pattern->getAllTypedText();
                if(!check_name(text)) {
                    return;
                }

                item.kind = CompletionItemKind::Snippet;
                item.edit.text = item.label;
                break;
            }

            case clang::CodeCompletionResult::RK_Macro: {
                if(!check_name(candidate.Macro->getName())) {
                    return;
                }

                item.kind = feature::CompletionItemKind::Unit;
                item.edit.text = item.label;
                break;
            }

            case clang::CodeCompletionResult::RK_Declaration: {
                auto declaration = candidate.Declaration;
                /// if(declaration)
                ///  auto name = getDeclName();
                if(!check_name(ast::name_of(declaration))) {
                    return;
                }

                /// Otherwise just add this declaration.
                item.kind = completion_kind(declaration);

                /// If bundle_overloads is enabled and this is a function, just
                /// increase the overload count.
                if(option.bundle_overloads && item.kind == CompletionItemKind::Function) {
                    llvm::SmallString<256> qualfied_name;
                    llvm::raw_svector_ostream os(qualfied_name);
                    declaration->printQualifiedName(os);

                    auto hash = llvm::xxh3_64bits(qualfied_name);
                    OverloadSet set{declaration, item.score, 1};
                    auto [it, success] = overloads.try_emplace(hash, set);
                    if(!success) {
                        it->second.count += 1;
                    }
                    return;
                }

                item.edit.text = render(declaration);
            }
        }

        items.emplace_back(std::move(item));
    }

    /// TODO: Handle dependent name with `TemplateResolver`.
    void handle_dependent() {
        /// For qualified name
        /// auto specifier = context.getCXXScopeSpecifier();
        /// auto NNS = specifier.value()->getScopeRep();

        /// For member access like a.c
        /// auto base = context.getBaseType();

        /// Preferred type.
        /// auto type = context.getPreferredType();
    }

    ~CompletionRender() {
        /// Add overload set.
        for(auto& [_, overload_set]: overloads) {
            CompletionItem item;
            item.label = ast::name_of(overload_set.first);
            item.kind = CompletionItemKind::Function;
            item.score = overload_set.score;
            item.edit.range = prefix.range;

            if(overload_set.count == 1) {
                item.edit.text = render(overload_set.first);
                /// TODO: Render function signature.
                item.description = "";
            } else {
                item.edit.text = item.label;
                item.description = "(...)";
            }

            items.emplace_back(std::move(item));
        }
    }

private:
    /// clang `Sema` ref.
    clang::Sema& sema;

    /// clang `Sema` ref.
    clang::ASTContext& context;

    /// The content of current completion file.
    llvm::StringRef content;

    /// The completion prefix.
    CompletionPrefix prefix;

    /// The fuzzy matcher to score results.
    FuzzyMatcher matcher;

    /// The code completion results.
    std::vector<CompletionItem>& items;

    /// The code completion context.
    clang::CodeCompletionContext& cc_context;

    /// A map between overload identifier hash and overloads count.
    llvm::DenseMap<std::uint64_t, OverloadSet> overloads;

    /// The code completion option.
    const config::CodeCompletionOption& option;
};

class CodeCompletionCollector final : public clang::CodeCompleteConsumer {
public:
    CodeCompletionCollector(std::uint32_t offset,
                            std::vector<CompletionItem>& items,
                            const config::CodeCompletionOption& option) :
        clang::CodeCompleteConsumer({}), offset(offset),
        info(std::make_shared<clang::GlobalCodeCompletionAllocator>()), items(items),
        option(option) {}

    clang::CodeCompletionAllocator& getAllocator() final {
        return info.getAllocator();
    }

    clang::CodeCompletionTUInfo& getCodeCompletionTUInfo() final {
        return info;
    }

    void ProcessCodeCompleteResults(clang::Sema& sema,
                                    clang::CodeCompletionContext cc_context,
                                    clang::CodeCompletionResult* candidates,
                                    unsigned candidates_count) final {
        // Results from recovery mode are generally useless, and the callback after
        // recovery (if any) is usually more interesting. To make sure we handle the
        // future callback from sema, we just ignore all callbacks in recovery mode,
        // as taking only results from recovery mode results in poor completion
        // results.
        // FIXME: in case there is no future sema completion callback after the
        // recovery mode, we might still want to provide some results (e.g. trivial
        // identifier-based completion).
        if(cc_context.getKind() == clang::CodeCompletionContext::CCC_Recovery) {
            return;
        }

        if(candidates_count == 0) {
            return;
        }

        /// FIXME: check Sema may run multiple times.
        auto& src_mgr = sema.getSourceManager();
        auto content = src_mgr.getBufferData(src_mgr.getMainFileID());

        CompletionRender render(sema, offset, content, items, cc_context, option);

        for(auto& candidate: llvm::make_range(candidates, candidates + candidates_count)) {
            render.process_candidate(candidate);
        }
    }

private:
    std::uint32_t offset;
    clang::CodeCompletionTUInfo info;
    std::vector<CompletionItem>& items;
    const config::CodeCompletionOption& option;
};

}  // namespace

std::vector<CompletionItem> code_complete(CompilationParams& params,
                                          const config::CodeCompletionOption& option) {
    std::vector<CompletionItem> items;
    auto& [file, offset] = params.completion;
    auto consumer = new CodeCompletionCollector(offset, items, option);

    if(auto info = complete(params, consumer)) {
        /// TODO: Handle error here.
    }

    return items;
}

}  // namespace clice::feature

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "feature/feature.h"
#include "semantic/ast_utility.h"
#include "support/fuzzy_matcher.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/Support/raw_ostream.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/Basic/CharInfo.h"
#include "clang/Sema/CodeCompleteConsumer.h"
#include "clang/Sema/Sema.h"

namespace clice::feature {

namespace {

namespace protocol = eventide::language::protocol;

struct CompletionPrefix {
    LocalSourceRange range;
    llvm::StringRef spelling;

    static auto from(llvm::StringRef content, std::uint32_t offset) -> CompletionPrefix {
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
            .range = LocalSourceRange(start, end),
            .spelling = content.substr(start, offset - start),
        };
    }
};

auto completion_kind(const clang::NamedDecl* decl) -> protocol::CompletionItemKind {
    if(llvm::isa<clang::NamespaceDecl, clang::NamespaceAliasDecl>(decl)) {
        return protocol::CompletionItemKind::Module;
    }

    if(llvm::isa<clang::FunctionDecl, clang::FunctionTemplateDecl>(decl)) {
        return protocol::CompletionItemKind::Function;
    }

    if(llvm::isa<clang::CXXMethodDecl,
                 clang::CXXConversionDecl,
                 clang::CXXDestructorDecl,
                 clang::CXXDeductionGuideDecl>(decl)) {
        return protocol::CompletionItemKind::Method;
    }

    if(llvm::isa<clang::CXXConstructorDecl>(decl)) {
        return protocol::CompletionItemKind::Constructor;
    }

    if(llvm::isa<clang::FieldDecl, clang::IndirectFieldDecl>(decl)) {
        return protocol::CompletionItemKind::Field;
    }

    if(llvm::isa<clang::VarDecl,
                 clang::ParmVarDecl,
                 clang::ImplicitParamDecl,
                 clang::BindingDecl,
                 clang::NonTypeTemplateParmDecl>(decl)) {
        return protocol::CompletionItemKind::Variable;
    }

    if(llvm::isa<clang::LabelDecl>(decl)) {
        return protocol::CompletionItemKind::Variable;
    }

    if(llvm::isa<clang::EnumDecl>(decl)) {
        return protocol::CompletionItemKind::Enum;
    }

    if(llvm::isa<clang::EnumConstantDecl>(decl)) {
        return protocol::CompletionItemKind::EnumMember;
    }

    if(llvm::isa<clang::RecordDecl,
                 clang::ClassTemplateDecl,
                 clang::ClassTemplateSpecializationDecl>(decl)) {
        return protocol::CompletionItemKind::Class;
    }

    if(llvm::isa<clang::TypedefNameDecl,
                 clang::TemplateTypeParmDecl,
                 clang::TemplateTemplateParmDecl,
                 clang::TypeAliasTemplateDecl,
                 clang::ConceptDecl>(decl)) {
        return protocol::CompletionItemKind::TypeParameter;
    }

    return protocol::CompletionItemKind::Text;
}

struct OverloadItem {
    protocol::CompletionItem item;
    float score = 0.0F;
    std::uint32_t count = 0;
};

class CodeCompletionCollector final : public clang::CodeCompleteConsumer {
public:
    CodeCompletionCollector(std::uint32_t offset,
                            PositionEncoding encoding,
                            std::vector<protocol::CompletionItem>& output,
                            const CodeCompletionOptions& options) :
        clang::CodeCompleteConsumer({}), offset(offset), encoding(encoding), output(output),
        options(options), info(std::make_shared<clang::GlobalCodeCompletionAllocator>()) {}

    clang::CodeCompletionAllocator& getAllocator() final {
        return info.getAllocator();
    }

    clang::CodeCompletionTUInfo& getCodeCompletionTUInfo() final {
        return info;
    }

    void ProcessCodeCompleteResults(clang::Sema& sema,
                                    clang::CodeCompletionContext context,
                                    clang::CodeCompletionResult* candidates,
                                    unsigned candidate_count) final {
        if(context.getKind() == clang::CodeCompletionContext::CCC_Recovery ||
           candidate_count == 0) {
            return;
        }

        auto& source_manager = sema.getSourceManager();
        auto content = source_manager.getBufferData(source_manager.getMainFileID());
        auto prefix = CompletionPrefix::from(content, offset);
        FuzzyMatcher matcher(prefix.spelling);

        PositionMapper converter(content, encoding);
        auto replace_range = protocol::Range{
            .start = converter.to_position(prefix.range.begin),
            .end = converter.to_position(prefix.range.end),
        };

        std::vector<protocol::CompletionItem> collected;
        collected.reserve(candidate_count);

        std::vector<OverloadItem> overloads;
        overloads.reserve(candidate_count);
        std::unordered_map<std::string, std::size_t> overload_index;

        auto build_item =
            [&](llvm::StringRef label, protocol::CompletionItemKind kind, llvm::StringRef insert) {
                protocol::CompletionItem item{
                    .label = label.str(),
                };
                item.kind = kind;

                protocol::TextEdit edit{
                    .range = replace_range,
                    .new_text = insert.empty() ? label.str() : insert.str(),
                };
                item.text_edit = std::move(edit);
                return item;
            };

        auto try_add = [&](llvm::StringRef label,
                           protocol::CompletionItemKind kind,
                           llvm::StringRef insert_text,
                           llvm::StringRef overload_key) {
            if(label.empty()) {
                return;
            }

            auto score = matcher.match(label);
            if(!score.has_value()) {
                return;
            }

            if(!overload_key.empty()) {
                auto [it, inserted] =
                    overload_index.try_emplace(overload_key.str(), overloads.size());
                if(inserted) {
                    auto item = build_item(label, kind, insert_text);
                    item.sort_text = std::format("{}", *score);
                    overloads.push_back({
                        .item = std::move(item),
                        .score = *score,
                        .count = 1,
                    });
                } else {
                    auto& existing = overloads[it->second];
                    existing.count += 1;
                    if(*score > existing.score) {
                        existing.score = *score;
                        existing.item.sort_text = std::format("{}", *score);
                    }
                }
                return;
            }

            auto item = build_item(label, kind, insert_text);
            item.sort_text = std::format("{}", *score);
            collected.push_back(std::move(item));
        };

        for(auto& candidate: llvm::make_range(candidates, candidates + candidate_count)) {
            switch(candidate.Kind) {
                case clang::CodeCompletionResult::RK_Keyword:
                    try_add(candidate.Keyword,
                            protocol::CompletionItemKind::Keyword,
                            candidate.Keyword,
                            "");
                    break;

                case clang::CodeCompletionResult::RK_Pattern: {
                    auto text = candidate.Pattern->getAllTypedText();
                    try_add(text, protocol::CompletionItemKind::Snippet, text, "");
                    break;
                }

                case clang::CodeCompletionResult::RK_Macro:
                    try_add(candidate.Macro->getName(),
                            protocol::CompletionItemKind::Unit,
                            candidate.Macro->getName(),
                            "");
                    break;

                case clang::CodeCompletionResult::RK_Declaration: {
                    auto* declaration = candidate.Declaration;
                    if(!declaration) {
                        break;
                    }

                    auto label = ast::name_of(declaration);
                    auto kind = completion_kind(declaration);

                    llvm::SmallString<256> qualified_name;
                    if(options.bundle_overloads && kind == protocol::CompletionItemKind::Function) {
                        llvm::raw_svector_ostream stream(qualified_name);
                        declaration->printQualifiedName(stream);
                    }

                    try_add(label, kind, label, qualified_name.str());
                    break;
                }
            }
        }

        for(auto& entry: overloads) {
            if(entry.count > 1) {
                entry.item.detail = "(...)";
            }
            collected.push_back(std::move(entry.item));
        }

        output.clear();
        output.swap(collected);
    }

private:
    std::uint32_t offset;
    PositionEncoding encoding;
    std::vector<protocol::CompletionItem>& output;
    const CodeCompletionOptions& options;
    clang::CodeCompletionTUInfo info;
};

}  // namespace

auto code_complete(CompilationParams& params,
                   const CodeCompletionOptions& options,
                   PositionEncoding encoding) -> std::vector<protocol::CompletionItem> {
    std::vector<protocol::CompletionItem> items;

    auto& [file, offset] = params.completion;
    (void)file;

    auto* consumer = new CodeCompletionCollector(offset, encoding, items, options);
    auto unit = complete(params, consumer);
    (void)unit;

    return items;
}

}  // namespace clice::feature

#include <algorithm>
#include <cassert>
#include <cctype>
#include <cstdint>
#include <format>
#include <optional>
#include <ranges>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "feature/feature.h"
#include "semantic/ast_utility.h"

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

auto to_lower(char c) -> char {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

auto fuzzy_score(llvm::StringRef pattern, llvm::StringRef word) -> std::optional<float> {
    if(pattern.empty()) {
        return 1.0F;
    }

    std::size_t scan = 0;
    float score = 0.0F;

    for(std::size_t i = 0; i < pattern.size(); ++i) {
        char p = to_lower(pattern[i]);

        bool found = false;
        for(std::size_t j = scan; j < word.size(); ++j) {
            if(to_lower(word[j]) != p) {
                continue;
            }

            score += (j == scan ? 2.0F : 1.0F);
            if(j == i) {
                score += 0.5F;
            }
            if(j == 0) {
                score += 0.5F;
            }

            scan = j + 1;
            found = true;
            break;
        }

        if(!found) {
            return std::nullopt;
        }
    }

    float denominator = static_cast<float>(pattern.size()) * 3.0F;
    return std::min(1.0F, score / denominator);
}

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

struct RankedItem {
    protocol::CompletionItem item;
    float score = 0.0F;
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

        PositionConverter converter(content, encoding);
        auto replace_range = protocol::Range{
            .start = converter.to_position(prefix.range.begin),
            .end = converter.to_position(prefix.range.end),
        };

        std::vector<RankedItem> ranked;
        ranked.reserve(candidate_count);

        std::unordered_map<std::string, std::size_t> index_by_key;
        std::unordered_map<std::string, std::uint32_t> overload_count;

        auto try_add = [&](llvm::StringRef label,
                           protocol::CompletionItemKind kind,
                           llvm::StringRef insert_text,
                           llvm::StringRef detail,
                           bool aggregate_overloads) {
            if(label.empty()) {
                return;
            }

            auto score = fuzzy_score(prefix.spelling, label);
            if(!score.has_value()) {
                return;
            }

            std::string key = aggregate_overloads
                                  ? label.str()
                                  : std::format("{}#{}", label, static_cast<int>(kind));

            if(auto it = index_by_key.find(key); it != index_by_key.end()) {
                auto& existing = ranked[it->second];
                if(*score > existing.score) {
                    existing.score = *score;
                    existing.item.sort_text = std::format("{:010.6f}", 1.0F - *score);
                    if(!detail.empty()) {
                        existing.item.detail = detail.str();
                    }
                }
                if(aggregate_overloads) {
                    overload_count[key] += 1;
                }
                return;
            }

            protocol::CompletionItem item{
                .label = label.str(),
            };
            item.kind = kind;
            item.sort_text = std::format("{:010.6f}", 1.0F - *score);

            if(!detail.empty()) {
                item.detail = detail.str();
            }

            protocol::TextEdit edit{
                .range = replace_range,
                .new_text = insert_text.empty() ? label.str() : insert_text.str(),
            };
            item.text_edit = std::move(edit);

            auto index = ranked.size();
            ranked.push_back({
                .item = std::move(item),
                .score = *score,
            });
            index_by_key.emplace(std::move(key), index);

            if(aggregate_overloads) {
                overload_count[ranked[index].item.label] = 1;
            }
        };

        for(auto& candidate: llvm::make_range(candidates, candidates + candidate_count)) {
            switch(candidate.Kind) {
                case clang::CodeCompletionResult::RK_Keyword:
                    try_add(candidate.Keyword,
                            protocol::CompletionItemKind::Keyword,
                            candidate.Keyword,
                            "",
                            false);
                    break;

                case clang::CodeCompletionResult::RK_Pattern: {
                    auto text = candidate.Pattern->getAllTypedText();
                    try_add(text, protocol::CompletionItemKind::Snippet, text, "", false);
                    break;
                }

                case clang::CodeCompletionResult::RK_Macro:
                    try_add(candidate.Macro->getName(),
                            protocol::CompletionItemKind::Constant,
                            candidate.Macro->getName(),
                            "macro",
                            false);
                    break;

                case clang::CodeCompletionResult::RK_Declaration: {
                    auto* declaration = candidate.Declaration;
                    if(!declaration) {
                        break;
                    }

                    auto label = ast::name_of(declaration);
                    auto kind = completion_kind(declaration);
                    auto detail = declaration->getDeclKindName();

                    bool aggregate =
                        options.bundle_overloads && kind == protocol::CompletionItemKind::Function;

                    try_add(label, kind, label, detail, aggregate);
                    break;
                }
            }
        }

        for(auto& [name, count]: overload_count) {
            if(count <= 1) {
                continue;
            }

            auto it = index_by_key.find(name);
            if(it == index_by_key.end()) {
                continue;
            }

            ranked[it->second].item.detail = "(...)";
        }

        std::ranges::sort(ranked, [](const RankedItem& lhs, const RankedItem& rhs) {
            if(lhs.score != rhs.score) {
                return lhs.score > rhs.score;
            }
            return lhs.item.label < rhs.item.label;
        });

        if(options.limit != 0 && ranked.size() > options.limit) {
            ranked.resize(options.limit);
        }

        output.clear();
        output.reserve(ranked.size());
        for(auto& item: ranked) {
            output.push_back(std::move(item.item));
        }
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

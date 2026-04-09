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

    if(llvm::isa<clang::CXXConstructorDecl>(decl)) {
        return protocol::CompletionItemKind::Constructor;
    }

    if(llvm::isa<clang::CXXMethodDecl,
                 clang::CXXConversionDecl,
                 clang::CXXDestructorDecl,
                 clang::CXXDeductionGuideDecl>(decl)) {
        return protocol::CompletionItemKind::Method;
    }

    if(llvm::isa<clang::FunctionDecl, clang::FunctionTemplateDecl>(decl)) {
        return protocol::CompletionItemKind::Function;
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

/// Extract the function signature (parameter list) from a CodeCompletionString.
/// Returns something like "(int x, float y)" for display in labelDetails.detail.
auto extract_signature(const clang::CodeCompletionString& ccs) -> std::string {
    std::string signature;
    bool in_parens = false;

    for(const auto& chunk: ccs) {
        using CK = clang::CodeCompletionString::ChunkKind;
        switch(chunk.Kind) {
            case CK::CK_LeftParen:
                in_parens = true;
                signature += '(';
                break;
            case CK::CK_RightParen:
                signature += ')';
                in_parens = false;
                break;
            case CK::CK_Placeholder:
            case CK::CK_CurrentParameter:
                if(in_parens && chunk.Text) {
                    signature += chunk.Text;
                }
                break;
            case CK::CK_Text:
            case CK::CK_Informative:
                if(in_parens && chunk.Text) {
                    signature += chunk.Text;
                }
                break;
            case CK::CK_LeftAngle:
                signature += '<';
                in_parens = true;
                break;
            case CK::CK_RightAngle:
                signature += '>';
                in_parens = false;
                break;
            case CK::CK_Comma:
                if(in_parens) {
                    signature += ", ";
                }
                break;
            default: break;
        }
    }

    return signature;
}

/// Extract the return type from a CodeCompletionString.
auto extract_return_type(const clang::CodeCompletionString& ccs) -> std::string {
    for(const auto& chunk: ccs) {
        if(chunk.Kind == clang::CodeCompletionString::CK_ResultType && chunk.Text) {
            return chunk.Text;
        }
    }
    return {};
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
            .start = *converter.to_position(prefix.range.begin),
            .end = *converter.to_position(prefix.range.end),
        };

        std::vector<protocol::CompletionItem> collected;
        collected.reserve(candidate_count);

        std::vector<OverloadItem> overloads;
        overloads.reserve(candidate_count);
        std::unordered_map<std::string, std::size_t> overload_index;

        bool prefix_starts_with_underscore = prefix.spelling.starts_with("_");

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
                           llvm::StringRef overload_key,
                           llvm::StringRef signature = {},
                           llvm::StringRef return_type = {}) {
            if(label.empty()) {
                return;
            }

            // Filter out _/__ prefixed internal symbols unless user typed _.
            if(!prefix_starts_with_underscore && label.starts_with("_")) {
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
                    if(!signature.empty() || !return_type.empty()) {
                        protocol::CompletionItemLabelDetails details;
                        if(!signature.empty()) {
                            details.detail = signature.str();
                        }
                        if(!return_type.empty()) {
                            details.description = return_type.str();
                        }
                        item.label_details = std::move(details);
                    }
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
            if(!signature.empty() || !return_type.empty()) {
                protocol::CompletionItemLabelDetails details;
                if(!signature.empty()) {
                    details.detail = signature.str();
                }
                if(!return_type.empty()) {
                    details.description = return_type.str();
                }
                item.label_details = std::move(details);
            }
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
                    bool is_callable = kind == protocol::CompletionItemKind::Function ||
                                       kind == protocol::CompletionItemKind::Method ||
                                       kind == protocol::CompletionItemKind::Constructor;
                    if(options.bundle_overloads && is_callable) {
                        llvm::raw_svector_ostream stream(qualified_name);
                        declaration->printQualifiedName(stream);
                    }

                    std::string signature;
                    std::string return_type;
                    auto* ccs =
                        candidate.CreateCodeCompletionString(sema,
                                                             context,
                                                             getAllocator(),
                                                             getCodeCompletionTUInfo(),
                                                             /*IncludeBriefComments=*/false);
                    if(ccs) {
                        signature = extract_signature(*ccs);
                        return_type = extract_return_type(*ccs);
                    }

                    try_add(label, kind, label, qualified_name.str(), signature, return_type);
                    break;
                }
            }
        }

        for(auto& entry: overloads) {
            if(entry.count > 1) {
                protocol::CompletionItemLabelDetails details;
                details.detail = std::format("(…) +{} overloads", entry.count);
                entry.item.label_details = std::move(details);
            }
            collected.push_back(std::move(entry.item));
        }

        // In bundle mode, deduplicate by label: when the same name appears as
        // both a class and its constructors/deduction guides, keep only the
        // highest-priority kind (Class > Function/Method > others).
        if(options.bundle_overloads) {
            auto kind_priority = [](protocol::CompletionItemKind k) -> int {
                switch(k) {
                    case protocol::CompletionItemKind::Class:
                    case protocol::CompletionItemKind::Struct: return 3;
                    case protocol::CompletionItemKind::Function:
                    case protocol::CompletionItemKind::Method: return 2;
                    case protocol::CompletionItemKind::Constructor: return 1;
                    default: return 0;
                }
            };

            std::unordered_map<std::string, std::size_t> label_index;
            std::vector<protocol::CompletionItem> deduped;
            deduped.reserve(collected.size());

            for(auto& item: collected) {
                auto [it, inserted] = label_index.try_emplace(item.label, deduped.size());
                if(inserted) {
                    deduped.push_back(std::move(item));
                } else {
                    auto& existing = deduped[it->second];
                    int old_prio = existing.kind.has_value() ? kind_priority(*existing.kind) : 0;
                    int new_prio = item.kind.has_value() ? kind_priority(*item.kind) : 0;
                    if(new_prio > old_prio) {
                        existing = std::move(item);
                    }
                }
            }
            collected.swap(deduped);
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

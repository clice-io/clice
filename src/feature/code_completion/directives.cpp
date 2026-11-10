module clice;

import :feature.code_completion.cursor;
import :feature.code_completion.directives;
import :feature.code_completion.ranking;
import :feature.code_completion.snippet;
import :feature.code_completion;
import :support.fuzzy_matcher;
import :syntax.position;

namespace clice::feature {

auto directive_completion(const CompletionName& completing,
                          const PositionMap& map,
                          PositionEncoding encoding,
                          const clang::LangOptions& dialect,
                          const CodeCompletionOptions& options,
                          const CompletionClient& client) -> protocol::CompletionList {
    // Clang's CodeCompletePreprocessorDirective, which a compile would
    // answer with, placeholders in braces.
    struct Pattern {
        llvm::StringLiteral name;
        llvm::StringLiteral arguments;
    };

    llvm::SmallVector<Pattern, 32> patterns = {
        {"if",     " {condition}"},
        {"ifdef",  " {macro}"    },
        {"ifndef", " {macro}"    },
    };
    if(completing.conditional == CompletionName::Conditional::Open) {
        patterns.append({
            {"elif",     " {condition}"},
            {"elifdef",  " {macro}"    },
            {"elifndef", " {macro}"    },
            {"else",     ""            },
        });
    }
    if(completing.conditional != CompletionName::Conditional::None) {
        patterns.push_back({"endif", ""});
    }
    patterns.append({
        {"include", R"( "{header}")"           },
        {"include", " <{header}>"              },
        {"define",  " {macro}"                 },
        {"define",  " {macro}({args})"         },
        {"undef",   " {macro}"                 },
        {"line",    " {number}"                },
        {"line",    R"( {number} "{filename}")"},
        {"error",   " {message}"               },
        {"pragma",  " {arguments}"             },
    });
    if(dialect.ObjC) {
        patterns.append({
            {"import", R"( "{header}")"},
            {"import", " <{header}>"   },
        });
    }
    patterns.append({
        {"include_next", R"( "{header}")"},
        {"include_next", " <{header}>"   },
        {"warning",      " {message}"    },
    });
    if(dialect.C23) {
        patterns.append({
            {"embed", R"( "{file}")"},
            {"embed", " <{file}>"   },
        });
    }

    LocalSourceRange typed(completing.replace.begin,
                           completing.replace.begin +
                               static_cast<std::uint32_t>(completing.prefix.size()));
    auto typed_range = map.range(typed, encoding);
    auto whole_range = map.range(completing.replace, encoding);
    bool snippets = options.statement_snippets && client.snippets;
    FuzzyMatcher matcher(completing.prefix);
    llvm::StringSet<> listed;
    std::vector<protocol::CompletionItem> items;
    for(auto& pattern: patterns) {
        auto score = matcher.match(pattern.name);
        // Without its placeholders a pattern is its name: the variants of
        // one directive collapse.
        if(!score || (!snippets && !listed.insert(pattern.name).second)) {
            continue;
        }
        protocol::CompletionItem item{.label = pattern.name.str()};
        item.kind = snippets ? protocol::CompletionItemKind::Snippet
                             : protocol::CompletionItemKind::Keyword;
        std::string text = pattern.name.str();
        if(snippets && !pattern.arguments.empty()) {
            std::string shown;
            unsigned placeholders = 0;
            for(auto rest = llvm::StringRef(pattern.arguments); !rest.empty();) {
                auto [literal, placeholder] = rest.split('{');
                auto [name, after] = placeholder.split('}');
                text += escape_snippet(literal);
                shown += literal;
                if(!name.empty()) {
                    placeholders += 1;
                    text += std::format("${{{}:{}}}", placeholders, name);
                    shown += name;
                }
                rest = after;
            }
            item.insert_text_format = protocol::InsertTextFormat::Snippet;
            item.label_details = protocol::CompletionItemLabelDetails{.detail = std::move(shown)};
        }
        if(client.insert_replace && typed != completing.replace) {
            item.text_edit = protocol::InsertReplaceEdit{
                .new_text = std::move(text),
                .insert = *typed_range,
                .replace = *whole_range,
            };
        } else {
            item.text_edit = protocol::TextEdit{.range = *whole_range, .new_text = std::move(text)};
        }
        item.sort_text = sort_text(*score * priority_weight(clang::CCP_CodePattern), std::nullopt);
        items.push_back(std::move(item));
    }
    return completion_list(std::move(items), options.limit);
}

}  // namespace clice::feature

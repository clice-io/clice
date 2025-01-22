#include "Index/Shared.h"
#include "Compiler/Semantic.h"
#include "Feature/SemanticTokens.h"

namespace clice::feature {

namespace {

class HighlightBuilder : public SemanticVisitor<HighlightBuilder> {
public:
    HighlightBuilder(ASTInfo& info, bool emitForIndex) :
        emitForIndex(emitForIndex), SemanticVisitor<HighlightBuilder>(info, true) {}

    void addToken(clang::FileID fid, const clang::Token& token, SymbolKind kind) {
        auto fake = clang::SourceLocation::getFromRawEncoding(1);
        LocalSourceRange range = {
            token.getLocation().getRawEncoding() - fake.getRawEncoding(),
            token.getEndLoc().getRawEncoding() - fake.getRawEncoding(),
        };

        auto& tokens = emitForIndex ? sharedResult[fid] : result;
        tokens.emplace_back(SemanticToken{
            .range = range,
            .kind = kind,
            .modifiers = {},
        });
    }

    void addToken(clang::SourceLocation location, SymbolKind kind, SymbolModifiers modifiers) {
        auto& SM = srcMgr;
        /// Always use spelling location.
        auto spelling = SM.getSpellingLoc(location);
        auto [fid, offset] = SM.getDecomposedLoc(spelling);

        /// If the spelling location is not in the interested file and not for index, skip it.
        if(fid != SM.getMainFileID() && !emitForIndex) {
            return;
        }

        auto& tokens = emitForIndex ? sharedResult[fid] : result;
        auto length = getTokenLength(SM, spelling);
        tokens.emplace_back(SemanticToken{
            .range = {offset, offset + length},
            .kind = kind,
            .modifiers = modifiers,
        });
    }

    /// Render semantic tokens from lexer. Note that we only render literal,
    /// directive, keyword, and comment tokens.
    void highlightFromLexer(clang::FileID fid) {
        auto& SM = srcMgr;
        auto content = getFileContent(SM, fid);
        auto& langOpts = pp.getLangOpts();

        /// Whether the token is after `#`.
        bool isAfterHash = false;
        /// Whether the token is in the header name.
        bool isInHeader = false;
        /// Whether the token is in the directive line.
        bool isInDirectiveLine = false;

        /// Use to distinguish whether the token is in a keyword.
        clang::IdentifierTable identifierTable(pp.getLangOpts());

        auto callback = [&](const clang::Token& token) -> bool {
            SymbolKind kind = SymbolKind::Invalid;

            switch(token.getKind()) {
                case clang::tok::comment: {
                    kind = SymbolKind::Comment;
                    break;
                }

                case clang::tok::numeric_constant: {
                    kind = SymbolKind::Number;
                    break;
                }

                case clang::tok::char_constant:
                case clang::tok::wide_char_constant:
                case clang::tok::utf8_char_constant:
                case clang::tok::utf16_char_constant:
                case clang::tok::utf32_char_constant: {
                    kind = SymbolKind::Character;
                    break;
                }

                case clang::tok::string_literal: {
                    if(isInHeader) {
                        isInHeader = false;
                        kind = SymbolKind::Header;
                    } else {
                        kind = SymbolKind::String;
                    }
                    break;
                }

                case clang::tok::wide_string_literal:
                case clang::tok::utf8_string_literal:
                case clang::tok::utf16_string_literal:
                case clang::tok::utf32_string_literal: {
                    kind = SymbolKind::String;
                    break;
                }

                case clang::tok::hash: {
                    if(token.isAtStartOfLine()) {
                        isAfterHash = true;
                        isInDirectiveLine = true;
                        kind = SymbolKind::Directive;
                    }
                    break;
                }

                case clang::tok::less: {
                    if(isInHeader) {
                        kind = SymbolKind::Header;
                    }
                    break;
                }

                case clang::tok::greater: {
                    if(isInHeader) {
                        isInHeader = false;
                        kind = SymbolKind::Header;
                    }
                    break;
                }

                case clang::tok::raw_identifier: {
                    auto spelling = token.getRawIdentifier();
                    if(isAfterHash) {
                        isAfterHash = false;
                        isInHeader = (spelling == "include");
                        kind = SymbolKind::Directive;
                    } else if(isInHeader) {
                        kind = SymbolKind::Header;
                    } else if(isInDirectiveLine) {
                        if(spelling == "defined") {
                            kind = SymbolKind::Directive;
                        }
                    } else {
                        /// Check whether the identifier is a keyword.
                        if(auto& II = identifierTable.get(spelling); II.isKeyword(langOpts)) {
                            kind = SymbolKind::Keyword;
                        }
                    }
                }

                default: {
                    break;
                }
            }

            /// Clear the directive line flag.
            if(token.isAtStartOfLine() && token.isNot(clang::tok::hash)) {
                isInDirectiveLine = false;
            }

            if(kind != SymbolKind::Invalid) {
                addToken(fid, token, kind);
            }

            return true;
        };

        tokenize(content, callback, false, &langOpts);
    }

    void handleDeclOccurrence(const clang::NamedDecl* decl,
                              RelationKind kind,
                              clang::SourceLocation location) {
        /// FIXME: Add modifiers.
        addToken(location, SymbolKind::from(decl), {});
    }

    void handleMacroOccurrence(const clang::MacroInfo* def,
                               RelationKind kind,
                               clang::SourceLocation location) {
        /// FIXME: Add modifiers.
        addToken(location, SymbolKind::Macro, {});
    }

    /// FIXME: handle module name.

    void merge(std::vector<SemanticToken>& tokens) {
        ranges::sort(tokens, refl::less, [](const auto& token) { return token.range; });
        /// FIXME: Resolve the overlapped tokens.
    }

    auto buildForFile() {
        highlightFromLexer(info.getInterestedFile());
        run();
        merge(result);
        return std::move(result);
    }

    auto buildForIndex() {
        for(auto fid: info.files()) {
            highlightFromLexer(fid);
        }

        run();

        for(auto& [fid, tokens]: sharedResult) {
            merge(tokens);
        }

        return std::move(sharedResult);
    }

private:
    std::vector<SemanticToken> result;
    index::Shared<std::vector<SemanticToken>> sharedResult;
    bool emitForIndex;
};

}  // namespace

index::Shared<std::vector<SemanticToken>> semanticTokens(ASTInfo& info) {
    return HighlightBuilder(info, true).buildForIndex();
}

proto::SemanticTokens toSemanticTokens(llvm::ArrayRef<SemanticToken> tokens,
                                       SourceConverter& SC,
                                       llvm::StringRef content,
                                       const config::SemanticTokensOption& option) {

    std::size_t lastLine = 0;
    std::size_t lastColumn = 0;

    for(auto& token: tokens) {}

    return {};
}

proto::SemanticTokens semanticTokens(ASTInfo& info,
                                     SourceConverter& SC,
                                     const config::SemanticTokensOption& option) {
    return {};
}

}  // namespace clice::feature

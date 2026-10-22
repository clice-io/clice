module;

#include "modules/prelude.h"

module clice:feature.index_check;

import :compile.compilation;
import :feature.feature;

/// Where what the index computes of a file — its semantic tokens, its
/// outline — differs from what its AST gives: the server answers from the
/// index until a file's AST is current, and both answers must be one.
namespace clice::feature {

/// One way the tokens the index computes differ from the AST's, at the
/// token it concerns (1-based line and column).
struct TokenDifference {
    std::uint32_t line = 0;
    std::uint32_t column = 0;
    std::string text;
    /// only_ast, only_index, range, kind or modifiers.
    std::string field;
    std::string ast;
    std::string index;
};

struct TokenCheck {
    std::uint32_t ast = 0;
    std::uint32_t index = 0;
    std::vector<TokenDifference> differences;
};

/// The file's semantic tokens as the AST gives them beside the ones the
/// index computes from the unit's own TU index, as the server serves a file
/// before its AST is current: tokens pair by where they begin.
auto check_semantic_tokens(CompilationUnit& unit, const SemanticTokensOptions& options)
    -> TokenCheck;

/// One way the outline the index computes differs from the AST's, at the
/// name of the entry it concerns (1-based line and column).
struct OutlineDifference {
    std::uint32_t line = 0;
    std::uint32_t column = 0;
    /// only_ast, only_index, name, kind, range, parent, detail or
    /// deprecated.
    std::string field;
    std::string ast;
    std::string index;
};

struct OutlineCheck {
    std::uint32_t ast = 0;
    std::uint32_t index = 0;
    std::vector<OutlineDifference> differences;
};

/// The file's outline as the AST gives it beside the one the index computes
/// from the unit's own TU index, as the server serves a file before its AST
/// is current: an entry of one matches the other's entry of the same name
/// range, the same name first. The index knows no signatures, so a detail
/// differs only where the index gives one.
auto check_document_symbols(CompilationUnit& unit, const DocumentSymbolsOptions& options)
    -> OutlineCheck;

}  // namespace clice::feature

#pragma once

#include "semantic/symbol.h"

#include "llvm/ADT/StringRef.h"
#include "clang/Basic/TokenKinds.h"

namespace clice::feature {

/// The classification of one token: a kind and its modifiers.
struct Classified {
    SymbolKind kind = SymbolKind::Invalid;
    std::uint32_t modifiers = 0;
};

/// Merge a candidate into the running classification: differing kinds
/// collapse to Conflict, and only the modifiers every candidate agrees on
/// survive — a token combining resolutions from several instantiations
/// must not depend on the order the instantiations were written in.
inline void combine(Classified& result, Classified candidate) {
    if(candidate.kind == SymbolKind::Invalid) {
        return;
    }
    if(result.kind == SymbolKind::Invalid) {
        result = candidate;
        return;
    }
    result.modifiers &= candidate.modifiers;
    if(result.kind != candidate.kind) {
        result.kind = SymbolKind::Conflict;
    }
}

/// Lexical classification of one token, shared by the AST semantic-token
/// collector (resolved spelled tokens) and the index projection (raw lex
/// plus identifier-table keyword resolution): both must color a keyword or
/// literal identically for the index output to stay a subset of the AST
/// output.
struct LexicalClass {
    SymbolKind kind = SymbolKind::Invalid;

    /// Whether the token reads as a word (identifier or keyword) — the
    /// directive state machines classify e.g. the name after `#` or
    /// `#define` only for word-like tokens.
    bool identifier_like = false;
};

/// Classify a token from its resolved kind and written spelling. `kind`
/// must have keywords resolved (a raw_identifier never classifies as a
/// keyword here — resolve it through an IdentifierTable first).
inline LexicalClass classify_lexical_kind(clang::tok::TokenKind kind, llvm::StringRef spelling) {
    LexicalClass result;
    result.identifier_like = clang::tok::isAnyIdentifier(kind);

    switch(kind) {
        case clang::tok::numeric_constant: result.kind = SymbolKind::Number; break;

        /// Character literals
        case clang::tok::char_constant:
        case clang::tok::wide_char_constant:
        case clang::tok::utf8_char_constant:
        case clang::tok::utf16_char_constant:
        case clang::tok::utf32_char_constant: result.kind = SymbolKind::Character; break;

        /// String literals
        case clang::tok::string_literal:
        case clang::tok::wide_string_literal:
        case clang::tok::utf8_string_literal:
        case clang::tok::utf16_string_literal:
        case clang::tok::utf32_string_literal: result.kind = SymbolKind::String; break;

        /// Fundamental and Clang/GNU builtin types; `__fp16` lexes as
        /// `kw_half`.
        case clang::tok::kw_bool:
        case clang::tok::kw_char:
        case clang::tok::kw_wchar_t:
        case clang::tok::kw_char8_t:
        case clang::tok::kw_char16_t:
        case clang::tok::kw_char32_t:
        case clang::tok::kw_double:
        case clang::tok::kw_float:
        case clang::tok::kw_int:
        case clang::tok::kw_long:
        case clang::tok::kw_short:
        case clang::tok::kw_signed:
        case clang::tok::kw_unsigned:
        case clang::tok::kw_void:
        case clang::tok::kw_half:
        case clang::tok::kw__BitInt:
        case clang::tok::kw__Bool:
        case clang::tok::kw__Complex:
        case clang::tok::kw__Decimal128:
        case clang::tok::kw__Decimal32:
        case clang::tok::kw__Decimal64:
        case clang::tok::kw__ExtInt:
        case clang::tok::kw__Float16:
        case clang::tok::kw__Imaginary:
        case clang::tok::kw___bf16:
        case clang::tok::kw___float128:
        case clang::tok::kw___ibm128:
        case clang::tok::kw___int64:
        case clang::tok::kw___int128: {
            result.kind = SymbolKind::Primitive;
            result.identifier_like = true;
            break;
        }

        /// PP directive hash
        case clang::tok::hash: break;

        default: {
            if(clang::tok::getKeywordSpelling(kind)) {
                result.kind = SymbolKind::Keyword;
                result.identifier_like = true;
                break;
            } else if(auto* punctuator = clang::tok::getPunctuatorSpelling(kind)) {
                /// Alternative operator spellings (and, or, not, ...) lex
                /// as their punctuator kinds but are written as words.
                if(!spelling.empty() && llvm::isAlpha(spelling.front()) && spelling != punctuator) {
                    result.kind = SymbolKind::Keyword;
                }
            }
            break;
        }
    }

    return result;
}

}  // namespace clice::feature

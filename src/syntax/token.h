#pragma once

#include <cassert>
#include <cstdint>
#include <tuple>

#include "llvm/ADT/StringRef.h"
#include "clang/Basic/SourceLocation.h"
#include "clang/Lex/Token.h"

namespace std {

template <>
struct tuple_size<clang::SourceRange> : std::integral_constant<std::size_t, 2> {};

template <>
struct tuple_element<0, clang::SourceRange> {
    using type = clang::SourceLocation;
};

template <>
struct tuple_element<1, clang::SourceRange> {
    using type = clang::SourceLocation;
};

}  // namespace std

namespace clang {

template <std::size_t I>
clang::SourceLocation get(clang::SourceRange range) {
    if constexpr(I == 0) {
        return range.getBegin();
    } else {
        return range.getEnd();
    }
}

}  // namespace clang

namespace clice {

struct LocalSourceRange {
    std::uint32_t begin = static_cast<std::uint32_t>(-1);
    std::uint32_t end = static_cast<std::uint32_t>(-1);

    constexpr bool operator==(const LocalSourceRange& other) const = default;

    constexpr std::uint32_t length() const {
        return end - begin;
    }

    constexpr bool contains(std::uint32_t offset) const {
        return offset >= begin && offset <= end;
    }

    constexpr bool intersects(const LocalSourceRange& other) const {
        return begin <= other.end && end >= other.begin;
    }

    constexpr bool valid() const {
        return begin != static_cast<std::uint32_t>(-1) && end != static_cast<std::uint32_t>(-1);
    }
};

using TokenKind = clang::tok::TokenKind;

struct Token {
    bool is_at_start_of_line = false;
    bool is_pp_keyword = false;
    TokenKind kind = clang::tok::unknown;
    LocalSourceRange range;

    bool valid() const {
        return range.valid();
    }

    llvm::StringRef name() const {
        return clang::tok::getTokenName(kind);
    }

    llvm::StringRef text(llvm::StringRef content) const {
        assert(range.valid() && "Invalid source range");
        return content.substr(range.begin, range.end - range.begin);
    }

    bool is_eod() const {
        return kind == clang::tok::eod;
    }

    bool is_eof() const {
        return kind == clang::tok::eof;
    }

    bool is_identifier() const {
        return kind == clang::tok::raw_identifier;
    }

    bool is_directive_hash() const {
        return is_at_start_of_line && kind == clang::tok::hash;
    }

    bool is_header_name() const {
        return kind == clang::tok::header_name;
    }
};

}  // namespace clice

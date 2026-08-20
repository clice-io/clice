#pragma once

/// The index vocabulary: row and symbol types shared by every layer —
/// builders accumulate them, blob readers hand them out, the project
/// table stores them.

#include <bit>
#include <cstdint>
#include <string>
#include <vector>

#include "semantic/symbol.h"
#include "support/bitmap.h"
#include "syntax/token.h"

#include "llvm/ADT/DenseMap.h"

namespace clice::index {

using Range = LocalSourceRange;
using SymbolHash = std::uint64_t;

/// Visibility scope of a symbol, determining which level of the multi-level
/// symbol table stores it.
enum class SymbolScope : std::uint8_t {
    /// Can be referenced from any TU (external linkage).  Stored in ProjectIndex.
    External = 0,
    /// Can be referenced across files within one TU but not across TUs
    /// (internal linkage: static, anonymous namespace).  Stored in the main
    /// file's Shard blob.
    TULocal = 1,
    /// Cannot be referenced from any other file (local variables, parameters,
    /// labels).  Stored in the defining file's Shard blob.
    FileLocal = 2,
};

struct Relation {
    /// The raw enum rather than the RelationKind wrapper: the wrapper's
    /// constructors hide it from reflection, and reflection is what lets a
    /// relation vector persist as one contiguous struct vector.
    RelationKind::Kind kind = RelationKind::Invalid;

    std::uint32_t padding = 0;

    LocalSourceRange range;

    SymbolHash target_symbol;

    constexpr void set_definition_range(LocalSourceRange range) {
        target_symbol = std::bit_cast<SymbolHash>(range);
    }

    constexpr auto definition_range() {
        return std::bit_cast<LocalSourceRange>(target_symbol);
    }
};

struct Occurrence {
    Range range;

    /// Hash of the symbol this occurrence names.
    SymbolHash target;

    friend bool operator==(const Occurrence&, const Occurrence&) = default;
};

/// One file's rows while a build accumulates them; encoded into a shard
/// blob (index/shard.h) at build end and consumed as bytes from then on.
struct FileIndex {
    /// The braces matter: fbs decode value-constructs map entries with
    /// `FileIndex{}`, and without an initializer this member would be
    /// copy-initialized from an empty list, which DenseMap's explicit
    /// default constructor rejects.
    llvm::DenseMap<SymbolHash, std::vector<Relation>> relations{};

    std::vector<Occurrence> occurrences;

    bool empty() const {
        return occurrences.empty() && relations.empty();
    }
};

struct Symbol {
    std::string name;

    SymbolKind kind;

    SymbolScope scope = SymbolScope::External;

    /// All files that referenced this symbol.
    Bitmap reference_files;

    friend bool operator==(const Symbol&, const Symbol&) = default;
};

using SymbolTable = llvm::DenseMap<SymbolHash, Symbol>;

/// A symbol's identity as a blob reader hands it out; the name borrows
/// the blob's bytes.
struct SymbolIdentity {
    llvm::StringRef name;
    SymbolKind kind;
    SymbolScope scope;
};

}  // namespace clice::index

#pragma once

/// @file tu_index.h
/// @brief Translation-unit-level index built from a single Clang AST.
///
/// TUIndex captures all symbol occurrences, inter-symbol relations, and include
/// graph information produced by compiling one translation unit. It is the
/// primary output of the indexing pipeline and can be serialized to/from a
/// FlatBuffer for disk persistence. At merge time, per-file FileIndex entries
/// are extracted and folded into the project-wide ProjectIndex and per-file
/// MergedIndex structures.

#include <array>
#include <bit>
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include "index/include_graph.h"
#include "semantic/relation_kind.h"
#include "semantic/symbol_kind.h"
#include "support/bitmap.h"

#include "llvm/Support/raw_ostream.h"

namespace clice::index {

using Range = LocalSourceRange;
using SymbolHash = std::uint64_t;

/// A typed edge between a symbol and another entity (symbol, source range, or definition).
/// The meaning of `target_symbol` depends on `kind`:
///   - For declarations/definitions: stores the definition range (bit-cast).
///   - For references: unused (zero).
///   - For inter-symbol relations (base/derived/caller/callee): the target symbol hash.
struct Relation {
    RelationKind kind;

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

/// A source-level occurrence of a symbol at a specific range.
/// Used for go-to-definition, find-references, and document highlighting.
struct Occurrence {
    /// Source range of this occurrence.
    Range range;

    /// Hash of the referenced symbol.
    SymbolHash target;

    friend bool operator==(const Occurrence&, const Occurrence&) = default;
};

/// Per-file index data: all symbol occurrences and relations within one file.
/// Each file touched by the translation unit gets its own FileIndex.
struct FileIndex {
    /// Relations grouped by the source symbol hash.
    llvm::DenseMap<SymbolHash, std::vector<Relation>> relations;

    /// Sorted, deduplicated list of symbol occurrences in this file.
    std::vector<Occurrence> occurrences;

    /// Compute a SHA-256 content hash over occurrences and relations.
    /// Used for change detection to avoid redundant index rewrites.
    std::array<std::uint8_t, 32> hash();
};

/// Global symbol metadata: name, kind, and the set of files that reference it.
/// Shared across all FileIndex entries within the same TUIndex and eventually
/// merged into the project-wide SymbolTable.
struct Symbol {
    std::string name;

    SymbolKind kind;

    /// Bitmap of path_ids for all files that contain a relation to this symbol.
    Bitmap reference_files;

    friend bool operator==(const Symbol&, const Symbol&) = default;
};

/// Maps symbol hashes to their metadata. Used both per-TU and project-wide.
using SymbolTable = llvm::DenseMap<SymbolHash, Symbol>;

struct TUIndex {
    /// The building timestamp of this file.
    std::chrono::milliseconds built_at;

    /// The include information of this file.
    IncludeGraph graph;

    SymbolTable symbols;

    llvm::DenseMap<clang::FileID, FileIndex> file_indices;

    /// File indices keyed by path_id, populated by from() for deserialized data.
    /// When built from AST, this is empty and file_indices (keyed by FileID) is used.
    llvm::DenseMap<std::uint32_t, FileIndex> path_file_indices;

    FileIndex main_file_index;

    /// Build a TUIndex by walking the AST with SemanticVisitor.
    /// Collects all symbol occurrences and relations, deduplicates and sorts
    /// them, then separates the main-file index from included-file indices.
    /// @param interested_only When true, only visit top-level decls (faster).
    static TUIndex build(CompilationUnitRef unit, bool interested_only = false);

    /// Serialize this index to a FlatBuffer binary format.
    /// File indices are keyed by path_id (converted from FileID via IncludeGraph).
    void serialize(llvm::raw_ostream& os) const;

    /// Deserialize a TUIndex from a FlatBuffer binary blob.
    /// Populates path_file_indices (keyed by path_id) instead of file_indices
    /// (keyed by FileID), since FileIDs are not meaningful outside a live AST.
    static TUIndex from(const void* data);
};

}  // namespace clice::index

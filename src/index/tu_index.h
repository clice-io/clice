#pragma once

#include <array>
#include <bit>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "index/include_graph.h"
#include "semantic/symbol.h"
#include "support/bitmap.h"

#include "kota/codec/macro.h"
#include "kota/meta/annotation.h"
#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/Support/raw_ostream.h"

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
    /// file's MergedIndex shard.
    TULocal = 1,
    /// Cannot be referenced from any other file (local variables, parameters,
    /// labels).  Stored in the defining file's MergedIndex shard.
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
    /// range of this occurrence.
    Range range;

    ///
    SymbolHash target;

    friend bool operator==(const Occurrence&, const Occurrence&) = default;
};

struct FileIndex {
    /// The braces matter: fbs decode value-constructs map entries with
    /// `FileIndex{}`, and without an initializer this member would be
    /// copy-initialized from an empty list, which DenseMap's explicit
    /// default constructor rejects.
    llvm::DenseMap<SymbolHash, std::vector<Relation>> relations{};

    std::vector<Occurrence> occurrences;

    void lookup(std::uint32_t offset, llvm::function_ref<bool(const Occurrence&)> callback) const;

    void lookup(SymbolHash symbol,
                RelationKind kind,
                llvm::function_ref<bool(const Relation&)> callback) const;

    std::array<std::uint8_t, 32> hash();
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

struct TUIndex {
    /// Persisted-blob schema version (index_format_version), stamped by
    /// serialize() and gated by from(). These blobs never touch disk — they
    /// travel worker→server over IPC — but a worker respawned after the
    /// binary on disk changed can be one build ahead of the server, and a
    /// layout change need not be structurally detectable.
    std::uint32_t format_version = 0;

    /// The building timestamp of this file.
    std::chrono::milliseconds built_at;

    /// The include information of this file.
    IncludeGraph graph;

    SymbolTable symbols;

    /// Build-time working state keyed by FileID — clang::FileID means nothing
    /// outside the compilation, so it never persists; serialize() converts it
    /// through graph.path_id.
    KOTATSU_ANNOTATE(skip = true)
    <llvm::DenseMap<clang::FileID, FileIndex>> file_indices;

    /// File indices keyed by path_id: populated from file_indices by
    /// serialize(), and directly by from() for deserialized data.
    llvm::DenseMap<std::uint32_t, FileIndex> path_file_indices;

    FileIndex main_file_index;

    /// Build the index for `unit`. With interested_only, only rows in
    /// the interested file are kept. Note that a full build over a unit
    /// compiled with a preamble PCH is not a production combination
    /// (background indexing compiles without PCH): rows landing in the
    /// PCH's loaded copy of the main file would serialize a second entry
    /// under the main path id.
    static TUIndex build(CompilationUnitRef unit, bool interested_only = false);

    /// Serialization reflects this object directly (path_file_indices is
    /// populated from file_indices first — hence non-const).
    void serialize(llvm::raw_ostream& os);

    /// Verify and deserialize a buffer; nullopt when structural
    /// verification fails, the format version differs, or a decoded path id
    /// falls outside the blob's own path table.
    static std::optional<TUIndex> from(llvm::StringRef data);
};

}  // namespace clice::index

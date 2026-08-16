#pragma once

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

    bool empty() const {
        return occurrences.empty() && relations.empty();
    }

    /// Content identity of the rows: xxh3 over the occurrences and the
    /// relation groups in ascending symbol order. Requires the canonical
    /// row order build() establishes (sorted, deduplicated); two files
    /// preprocessed identically hash equal, and that equality is what
    /// deduplicates variants across compilation contexts.
    std::uint64_t rows_hash() const;
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

/// One file's rows on the wire: the hash first, so the master can skip the
/// nested decode for rows it already stores, and the rows themselves as a
/// self-contained nested blob decoded per miss.
struct FileSection {
    std::uint32_t path_id = 0;

    /// FileIndex::rows_hash of the nested rows.
    std::uint64_t rows_hash = 0;

    /// Nested fbs FileIndex blob (TUIndex::decode_rows).
    std::vector<std::uint8_t> rows;
};

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

    /// The interested file's rows, used in memory (sessions, preamble
    /// state). serialize() moves it into its wire section for the duration
    /// of the write, so the reflected field always travels empty.
    FileIndex main_file_index;

    /// The wire form of the per-file rows: populated by serialize() from
    /// file_indices and main_file_index (the interested file's section is
    /// last), kept raw by from(). Files whose rows are empty get no
    /// section — no rows means no contribution.
    std::vector<FileSection> sections;

    /// Build the index for `unit`. With interested_only, only rows in
    /// the interested file are kept. Note that a full build over a unit
    /// compiled with a preamble PCH is not a production combination
    /// (background indexing compiles without PCH): rows landing in the
    /// PCH's loaded copy of the main file would serialize a second entry
    /// under the main path id.
    static TUIndex build(CompilationUnitRef unit, bool interested_only = false);

    /// Serialization reflects this object directly (sections are populated
    /// from the row state first — hence non-const).
    void serialize(llvm::raw_ostream& os);

    /// Verify and deserialize a buffer; nullopt when structural
    /// verification fails, the format version differs, or a decoded path id
    /// falls outside the blob's own path table. Section rows stay raw —
    /// decode them per file with decode_rows.
    static std::optional<TUIndex> from(llvm::StringRef data);

    /// The interested file's wire section, or nullptr when its rows were
    /// empty.
    const FileSection* main_section() const;

    /// Verify and decode one section's rows; nullopt for a corrupt nested
    /// blob.
    static std::optional<FileIndex> decode_rows(const FileSection& section);
};

/// A symbol's identity as a merge consumer needs it; the name borrows the
/// wire buffer.
struct SymbolIdentity {
    llvm::StringRef name;
    SymbolKind kind;
    SymbolScope scope;
};

/// Zero-copy reader over a serialized TUIndex, for the master's merge path:
/// the graph and the per-file rows hashes are read straight off the wire,
/// section rows are decoded only for actual misses, and symbol names are
/// touched only when a symbol is genuinely new to the global table. The
/// view borrows the wire bytes; keep them alive while using it.
///
/// TUIndex::from stays the full-decode entry for consumers that need the
/// whole object (sessions, tests).
class TUIndexView {
public:
    /// Verify the buffer, gate the format version, and bound every path id
    /// the graph and sections carry. Symbol reference-file ids are NOT
    /// validated here — iterate_symbols hands them out raw and the consumer
    /// bounds them (decoding every bitmap twice just to validate would
    /// defeat the view).
    static std::optional<TUIndexView> from(llvm::StringRef data);

    std::int64_t built_at() const;

    std::uint32_t path_count() const;

    llvm::StringRef path(std::uint32_t id) const;

    std::uint64_t path_hash(std::uint32_t id) const;

    std::uint32_t location_count() const;

    IncludeLocation location(std::uint32_t i) const;

    std::uint32_t section_count() const;

    std::uint32_t section_path(std::uint32_t i) const;

    std::uint64_t section_rows_hash(std::uint32_t i) const;

    /// Verify and decode one section's rows straight from the wire buffer.
    std::optional<FileIndex> decode_section_rows(std::uint32_t i) const;

    /// The section index of the interested file (path_count() - 1), or
    /// nullopt when its rows were empty.
    std::optional<std::uint32_t> main_section_index() const;

    /// Visit every symbol: hash, identity, and the raw serialized
    /// reference-files bitmap (a read_bitmap'able portable image).
    void iterate_symbols(
        llvm::function_ref<void(SymbolHash, const SymbolIdentity&, llvm::StringRef bitmap)>
            callback) const;

    /// Look up one symbol's identity by hash.
    std::optional<SymbolIdentity> find_symbol(SymbolHash hash) const;

private:
    explicit TUIndexView(llvm::StringRef data) : data(data) {}

    /// The verified wire bytes; accessors rebuild the (pointer-sized) fbs
    /// view from them on demand.
    llvm::StringRef data;
};

}  // namespace clice::index

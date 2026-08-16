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

/// One file's rows on the wire: a self-contained single-variant shard
/// blob (index/shard.h), stored verbatim by the master when the variant
/// is new and merged byte-for-byte otherwise. `hash` is xxh3 of `blob` —
/// the variant's identity — so the master can skip blobs it already
/// stores without touching their bytes.
struct FileSection {
    std::uint32_t path_id = 0;

    std::uint64_t hash = 0;

    std::vector<std::uint8_t> blob;
};

/// What indexing one TU produced, in transit from worker to master: the
/// include graph (interned into a manifest), the TU's symbol table with
/// per-symbol reference files (merged into the project table), and one
/// shard blob per file that received rows (stored or merged into the
/// file's disk shard). Never persisted itself; it travels worker→server
/// over IPC and is dismantled into the three persistent layers on
/// arrival.
struct TUIndex {
    /// Wire schema version (index_format_version), stamped by serialize()
    /// and gated by from(). A worker respawned after the binary on disk
    /// changed can be one build ahead of the server, and a layout change
    /// need not be structurally detectable.
    std::uint32_t format_version = 0;

    /// The building timestamp of this file.
    std::chrono::milliseconds built_at;

    /// The include information of this file.
    IncludeGraph graph;

    SymbolTable symbols;

    /// One entry per file with rows, ascending by path id; the interested
    /// file (the last path id) comes last. Rows of a header entered
    /// several times are one union blob. Files whose rows are empty get
    /// no section — no rows means no contribution.
    std::vector<FileSection> sections;

    /// Build the index for `unit`. With interested_only, only rows in
    /// the interested file are kept.
    static TUIndex build(CompilationUnitRef unit, bool interested_only = false);

    /// Serialization reflects this object directly.
    void serialize(llvm::raw_ostream& os);

    /// Verify and deserialize a buffer; nullopt when structural
    /// verification fails, the format version differs, or a decoded path id
    /// falls outside the blob's own path table. Section blobs stay raw —
    /// wrap them in a Shard per file to query them.
    static std::optional<TUIndex> from(llvm::StringRef data);

    /// The interested file's wire section, or nullptr when its rows were
    /// empty.
    const FileSection* main_section() const;
};

/// A symbol's identity as a merge consumer needs it; the name borrows the
/// wire buffer.
struct SymbolIdentity {
    llvm::StringRef name;
    SymbolKind kind;
    SymbolScope scope;
};

/// Zero-copy reader over a serialized TUIndex, for the master's merge path:
/// the graph, the per-file blob hashes and the blob bytes themselves are
/// read straight off the wire — a new variant's bytes are sliced out and
/// written or merged without ever decoding the envelope around them — and
/// symbol names are touched only when a symbol is genuinely new to the
/// global table. The view borrows the wire bytes; keep them alive while
/// using it.
///
/// TUIndex::from stays the full-decode entry for consumers that need the
/// whole object (sessions, tests).
class TUIndexView {
public:
    /// Verify the buffer, gate the format version, and bound every path id
    /// the graph and sections carry. Symbol reference-file ids are NOT
    /// validated here — iterate_symbols hands them out raw and the consumer
    /// bounds them (decoding every bitmap twice just to validate would
    /// defeat the view). Section blob bytes are not verified either;
    /// Shard::from_bytes verifies each blob the consumer actually uses.
    static std::optional<TUIndexView> from(llvm::StringRef data);

    std::int64_t built_at() const;

    std::uint32_t path_count() const;

    llvm::StringRef path(std::uint32_t id) const;

    std::uint64_t path_hash(std::uint32_t id) const;

    std::uint32_t location_count() const;

    IncludeLocation location(std::uint32_t i) const;

    std::uint32_t section_count() const;

    std::uint32_t section_path(std::uint32_t i) const;

    std::uint64_t section_hash(std::uint32_t i) const;

    /// One section's shard blob bytes, borrowing the wire buffer.
    llvm::StringRef section_blob(std::uint32_t i) const;

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

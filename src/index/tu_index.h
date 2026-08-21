#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "feature/feature.h"
#include "index/include_graph.h"
#include "index/shard.h"
#include "index/types.h"

#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/MemoryBuffer.h"

namespace clice {

class CompilationUnitRef;

}

namespace clice::index {

/// Index one TU and encode the result as its envelope bytes: the include
/// graph (interned into a manifest), the TU's symbol table with
/// per-symbol reference files (merged into the project table), and one
/// self-contained shard blob per file that received rows (stored or
/// merged into the file's disk shard). Rows of a header entered several
/// times are one union blob. The envelope travels worker→server over IPC
/// and is dismantled into the three persistent layers on arrival. With
/// interested_only, only rows in the interested file are kept.
std::string build_tu_index(CompilationUnitRef unit, bool interested_only = false);

/// The preamble variant: a preamble is a TU cut off at the preamble
/// bound, and its index is the same envelope — persisted verbatim as the
/// PCH's `.pch.idx` pair — plus the preamble-specific fields ordinary
/// envelopes leave empty: the identity of the exact preamble text, and
/// the PCH-derived feature state spliced into main-file results
/// (document links, inactive regions, the open conditional stack at the
/// bound).
std::string build_preamble_index(CompilationUnitRef unit,
                                 llvm::ArrayRef<feature::DocumentLink> links,
                                 llvm::ArrayRef<std::uint32_t> inactive_regions,
                                 llvm::ArrayRef<std::uint8_t> open_conditionals);

/// Zero-copy reader over an envelope: the graph, the per-file blob hashes
/// and the blob bytes themselves are read straight off the wire — a new
/// variant's bytes are sliced out and written or merged without ever
/// decoding the envelope around them — and symbol names are touched only
/// when a consumer genuinely needs them. Whole-envelope accessors on an
/// empty reader answer empty/zero; per-element accessors require a valid
/// index, and no index is valid on an empty reader (every count is 0).
class TUIndex {
public:
    TUIndex() = default;

    /// Wrap verified envelope bytes without owning them (the caller keeps
    /// the bytes alive). Verification gates the format version and bounds
    /// every path id the graph and sections carry; corrupt bytes load as
    /// an empty reader. Symbol reference-file ids are NOT validated —
    /// iterate_symbols hands them out raw and the consumer bounds them.
    /// Section blob bytes are verified per section: structurally by
    /// shard_of on first use, or hash-checked and wrapped by
    /// shards_verify in one pass.
    static TUIndex from_bytes(llvm::StringRef data);

    /// Adopt an owning buffer of envelope bytes (a mapped `.pch.idx`, a
    /// session's IPC result). Same verification as from_bytes.
    static TUIndex from_buffer(std::unique_ptr<llvm::MemoryBuffer> buffer);

    /// Whether this reader holds an envelope.
    bool loaded() const {
        return !data.empty();
    }

    /// The envelope bytes backing this reader.
    llvm::StringRef bytes() const {
        return data;
    }

    std::int64_t built_at() const;

    /// The interested file's path is always the last id, by IncludeGraph
    /// convention.
    std::uint32_t path_count() const;

    llvm::StringRef path(std::uint32_t id) const;

    std::uint64_t path_hash(std::uint32_t id) const;

    std::uint32_t location_count() const;

    IncludeLocation location(std::uint32_t i) const;

    std::uint32_t section_count() const;

    std::uint32_t section_path(std::uint32_t i) const;

    std::uint64_t section_hash(std::uint32_t i) const;

    /// One section's shard blob bytes, borrowing the envelope.
    llvm::StringRef section_blob(std::uint32_t i) const;

    /// The section holding `path_id`'s rows, or nullopt when the file had
    /// none (no rows means no contribution). Sections ascend by path id.
    std::optional<std::uint32_t> section_of(std::uint32_t path_id) const;

    /// A reader over `path_id`'s rows, wrapped on first use and cached
    /// for the envelope's lifetime (the wrap verifies the blob and later
    /// materializes its line table). An empty shard when the file has no
    /// section or its blob fails verification.
    const Shard& shard_of(std::uint32_t path_id) const;

    /// Wrap every section's blob in one pass, checking its bytes against
    /// the recorded section hash on top of structural verification — the
    /// load gate for persisted envelopes, where a corrupt blob must read
    /// as "pair missing" and rebuild instead of silently serving wrong
    /// rows or nothing.
    bool shards_verify() const;

    /// Visit every symbol: hash, identity, and the raw serialized
    /// reference-files bitmap (a read_bitmap'able portable image).
    /// Return false from the callback to stop.
    void iterate_symbols(
        llvm::function_ref<bool(SymbolHash, const SymbolIdentity&, llvm::StringRef bitmap)>
            callback) const;

    /// Look up one symbol's identity by hash.
    std::optional<SymbolIdentity> find_symbol(SymbolHash hash) const;

    /// Whether `text` still begins with the exact preamble this envelope
    /// was built from — the gate for serving preamble-derived state
    /// against a live buffer (the rows are offsets into that prefix).
    /// Compared by hash: the text itself is not stored. Always false for
    /// an ordinary envelope.
    bool matches_prefix(llvm::StringRef text) const;

    /// Document links of the preamble region, materialized from the
    /// envelope; empty for an ordinary one.
    std::vector<feature::DocumentLink> links() const;

    /// Inactive regions within the preamble (flat begin/end offset
    /// pairs); empty for an ordinary envelope. Borrows the envelope.
    llvm::ArrayRef<std::uint32_t> inactive_regions() const;

    /// Conditional stack still open at the preamble bound; empty for an
    /// ordinary envelope. Borrows the envelope.
    llvm::ArrayRef<std::uint8_t> open_conditionals() const;

private:
    /// The verified envelope bytes (owned iff `owned` is set); accessors
    /// rebuild the (pointer-sized) fbs view from them on demand.
    std::unique_ptr<llvm::MemoryBuffer> owned;
    llvm::StringRef data;

    /// Lazily wrapped per-section readers; the envelope is immutable for
    /// the reader's lifetime, so the cache never invalidates.
    mutable std::vector<Shard> shards;
};

}  // namespace clice::index

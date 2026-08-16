#pragma once

#include <memory>
#include <span>
#include <string>
#include <vector>

#include "feature/feature.h"
#include "index/shard.h"
#include "index/tu_index.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"

namespace clice::index {

/// On-disk PreambleIndex blob schema version (the PCH's `.pch.idx` pair).
/// Bump whenever the persisted layout (its reflected repr or the nested
/// shard blob format) changes; a blob carrying a different value loads as
/// "missing" and the PCH pair is rebuilt. cache.json records it so a
/// version change is caught at load time instead of on the first overlay
/// query.
constexpr inline std::uint32_t preamble_format_version = 6;

/// All master-visible state derived from one PCH build.
///
/// The stateless worker serializes it next to the PCH blob (the store's
/// `.pch.idx` pair) and the master opens it as a memory-mapped blob:
/// queries run directly on the serialized data, nothing is deserialized up
/// front. It carries the preamble's full symbol index — one shard blob per
/// header the PCH covers plus the main file's preamble region, the same
/// encoding every other holder of a file's rows uses — and the
/// PCH-derived feature state that is spliced into main-file results:
/// document links, inactive regions and the open conditional stack at the
/// preamble bound.
///
/// Lifecycle equals the PCH's: the pair is committed, hit and evicted
/// together, so no separate invalidation is needed — a preamble change or
/// a stale dependency rebuilds both.
///
/// The preamble cannot change while its PCH lives, so feature results are
/// computed once here — by the only live AST that ever sees the preamble —
/// and replayed on every request. New preamble-region features add
/// precomputed rows to this blob and define a per-feature merge with the
/// live results of the rest of the file.
class PreambleIndex {
public:
    /// A file entry handed to lookup callbacks: everything needed to turn
    /// a byte-offset hit into an LSP location. Views borrow the mapped
    /// blob; keep the PreambleIndex alive while using them.
    struct File {
        llvm::StringRef path;

        /// Empty for pure-ASCII content, which the blob does not store —
        /// byte offsets are already UTF-16 column offsets there.
        llvm::StringRef content;

        std::uint32_t content_size = 0;

        std::span<const std::uint32_t> line_starts;
    };

    /// Serialize a preamble compilation's state. `index` must be built
    /// over the preamble unit with interested_only=false; its sections
    /// (one shard blob per covered file) are embedded verbatim. Taken by
    /// value and consumed: the blob borrows the index's bytes and names
    /// for the duration of the write.
    static void serialize(CompilationUnitRef unit,
                          TUIndex index,
                          llvm::ArrayRef<feature::DocumentLink> links,
                          llvm::ArrayRef<std::uint32_t> inactive_regions,
                          llvm::ArrayRef<std::uint8_t> open_conditionals,
                          llvm::raw_ostream& os);

    /// Open a blob from disk (memory-mapped). Returns nullptr when the
    /// file is unreadable, structurally invalid, written by a different
    /// format version, or any embedded shard blob fails verification —
    /// callers treat all of these as a PCH cache miss.
    static std::shared_ptr<PreambleIndex> load(llvm::StringRef path);

    /// Iterate relations of `symbol` matching `kind` across all header
    /// entries. Return false from the callback to stop. This is the only
    /// query shape overlays serve: hash-anchored answering. Discovery
    /// inputs (by name, by path and line) are the disk index's job.
    void lookup(SymbolHash symbol,
                RelationKind kind,
                llvm::function_ref<bool(const File&, const Relation&)> callback) const;

    /// Path of the file whose preamble built this blob. Files with
    /// identical preambles share one PCH (the key excludes the source
    /// path), but the preamble entry carries file-local symbol identities
    /// — macro USRs embed the source path — so its lookups must be scoped
    /// to this file. Borrows the mapped blob.
    llvm::StringRef source_path() const;

    /// Whether `text` still begins with the exact preamble this blob was
    /// built from — the gate for serving preamble-derived state against a
    /// live buffer (the rows are offsets into that prefix). Compared by
    /// hash: the text itself is not stored.
    bool matches_prefix(llvm::StringRef text) const;

    /// Occurrence lookup in the source file's preamble region (buffer
    /// offsets below the preamble bound).
    void lookup_preamble(std::uint32_t offset,
                         llvm::function_ref<bool(const Occurrence&)> callback) const;

    /// Relations of `symbol` in the source file's preamble region.
    void lookup_preamble(SymbolHash symbol,
                         RelationKind kind,
                         llvm::function_ref<bool(const Relation&)> callback) const;

    /// Look up a symbol's name and kind in the blob's symbol table.
    bool find_symbol(SymbolHash hash, std::string& name, SymbolKind& kind) const;

    /// Document links of the preamble region, materialized from the blob.
    std::vector<feature::DocumentLink> links() const;

    /// Inactive regions within the preamble (flat begin/end offset pairs).
    /// Borrows the mapped blob.
    llvm::ArrayRef<std::uint32_t> inactive_regions() const;

    /// Conditional stack still open at the preamble bound. Borrows the
    /// mapped blob.
    llvm::ArrayRef<std::uint8_t> open_conditionals() const;

    /// Size of the mapped blob in bytes (memory accounting gauge).
    std::size_t size() const {
        return buffer ? buffer->getBufferSize() : 0;
    }

private:
    PreambleIndex() = default;

    std::unique_ptr<llvm::MemoryBuffer> buffer;

    /// One reader per header entry, wrapping the mapped blob's bytes;
    /// line-start caches accumulate here across queries. Paths borrow the
    /// mapped blob, parallel to the shards.
    std::vector<Shard> file_shards;
    std::vector<llvm::StringRef> file_paths;

    /// The source file's preamble-region rows; an empty shard when the
    /// region had none.
    Shard preamble_shard;
};

}  // namespace clice::index

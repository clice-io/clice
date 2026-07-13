#pragma once

#include <memory>
#include <span>
#include <string>
#include <vector>

#include "feature/document_link.h"
#include "index/tu_index.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"

namespace clice::index {

/// On-disk PreambleState blob schema version (the PCH's `.pch.idx` pair).
/// Bump whenever `schema.fbs` changes the PreambleState layout; a blob
/// carrying a different value loads as "missing" and the PCH pair is
/// rebuilt. cache.json records it so a version change is caught at load
/// time instead of on the first overlay query.
constexpr inline std::uint32_t preamble_format_version = 3;

/// All master-visible state derived from one PCH build.
///
/// The stateless worker serializes it next to the PCH blob (the store's
/// `.pch.idx` pair) and the master opens it as a memory-mapped FlatBuffer:
/// queries run directly on the serialized data, nothing is deserialized up
/// front. It carries the preamble's full symbol index — every header the
/// PCH covers plus the main file's preamble region — with per-file content
/// and line starts for position mapping (mirroring MergedIndex shards),
/// and the PCH-derived feature state that is spliced into main-file
/// results: document links, inactive regions and the open conditional
/// stack at the preamble bound.
///
/// Lifecycle equals the PCH's: the pair is committed, hit and evicted
/// together, so no separate invalidation is needed — a preamble change or
/// a stale dependency rebuilds both.
class PreambleState {
public:
    /// A file entry handed to lookup callbacks: everything needed to turn
    /// a byte-offset hit into an LSP location. Views borrow the mapped
    /// blob; keep the PreambleState alive while using them.
    struct File {
        llvm::StringRef path;
        llvm::StringRef content;
        std::span<const std::uint32_t> line_starts;
    };

    /// Serialize a preamble compilation's state. `index` must be built
    /// over the preamble unit with interested_only=false and its
    /// main_file_index intact (it holds the preamble region's own
    /// occurrences — macro definitions and references before the bound).
    static void serialize(CompilationUnitRef unit,
                          const TUIndex& index,
                          llvm::ArrayRef<feature::DocumentLink> links,
                          llvm::ArrayRef<std::uint32_t> inactive_regions,
                          llvm::ArrayRef<std::uint8_t> open_conditionals,
                          llvm::raw_ostream& os);

    /// Open a blob from disk (memory-mapped). Returns nullptr when the
    /// file is unreadable, structurally invalid or written by a different
    /// format version — callers treat all of these as a PCH cache miss.
    static std::shared_ptr<PreambleState> load(llvm::StringRef path);

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

    /// The exact preamble text this blob was built from. Consumers serve
    /// preamble-entry rows only while the live buffer still starts with
    /// it — the rows are buffer offsets into this prefix. Borrows the
    /// mapped blob.
    llvm::StringRef preamble_content() const;

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

private:
    explicit PreambleState(std::unique_ptr<llvm::MemoryBuffer> buffer);

    std::unique_ptr<llvm::MemoryBuffer> buffer;
};

}  // namespace clice::index

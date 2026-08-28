#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "index/tu_index.h"
#include "sched/workspace.h"

#include "kota/codec/visit/common.h"
#include "llvm/ADT/DenseMap.h"

namespace clice {

/// Defined in sched/context.h — the resolver reports where
/// the compile command came from; the projection only stores the verdict.
enum class CommandSource : std::uint8_t;

/// The publishable products of the most recent compilation (materialized
/// whole-document feature results). The data lives in the projection; the
/// AST family's on_output signal only wakes the push path up — a missed
/// signal is harmless, and with no transport connected the output simply
/// stays put.
struct CompileOutput {
    /// Document version the compile ran against; empty on the clear path
    /// (a failed compile publishes empty diagnostics without a version).
    std::optional<int> version;

    /// How the compile command was obtained; the push path merges a
    /// guidance diagnostic when the command was guessed.
    CommandSource source;

    /// Worker-produced raw diagnostics (unformatted); empty on failure.
    kota::codec::RawValue diagnostics;

    /// First phantom line introduced by suffix include injection —
    /// diagnostics at or past it describe text the user cannot see.
    std::optional<std::uint32_t> line_limit;
};

/// One open document's compilation products, owned by the AST family and
/// read by everything that used to read them off the Session. Immutable
/// once published: every change installs a fresh projection (copy-on-write
/// over the cheap members), so a reader that copied the shared_ptr may
/// keep using it across suspension points.
struct ASTProjection {
    /// Content key into Workspace.pch_cache for the document's PCH, if
    /// any. The PCH itself is owned by Workspace (shared,
    /// content-addressed); whether its preamble-derived state still
    /// describes the buffer is checked against the blob's stored preamble
    /// text at the point of use.
    std::optional<std::string> pch_key;

    /// The latest compilation's index envelope; null until a compile
    /// lands index data. NOT merged into Workspace.project_index — that
    /// only gets disk-derived data from background indexing.
    std::shared_ptr<index::TUIndex> index;

    /// Publishable products of the latest compilation, kept for the
    /// transport push path (see CompileOutput).
    std::optional<CompileOutput> output;

    /// The interested file's rows within `index` (an empty shard when the
    /// compile produced none). Callers gate on index_current, which
    /// implies a loaded envelope.
    const index::Shard& file_rows() const {
        return index->shard_of(index->path_count() - 1);
    }
};

/// The AST family's per-document table: projections plus the freshness
/// state that used to live on the Session (ast_dirty, dirty_epoch). Owned
/// and written by the family and its facade; readers (IndexQuery,
/// FeatureRouter, transports) only look things up. A separate type so
/// read-side tests can populate one without the family's machinery.
struct ASTProjectionTable {
    struct Entry {
        std::shared_ptr<const ASTProjection> projection;

        /// Dependency snapshot from the last successful AST compilation,
        /// used for two-layer staleness detection (mtime + content hash).
        /// Kept out of the immutable projection: a passing staleness
        /// check repairs the snapshot's stat fast paths in place, and no
        /// reader outside the family consumes it.
        std::optional<DepsSnapshot> deps;

        /// Whether the projection describes the current buffer: the last
        /// compile landed and no invalidation arrived since. The
        /// ast_dirty flag of the old world, inverted, with its single
        /// write points in the family (round landing and invalidation).
        bool current = false;

        /// Bumped by every invalidation of this document. The stateless
        /// forward path snapshots it before its PCH acquisition and may
        /// adopt the key only if it is unchanged — the dirty_epoch half
        /// of the retired pch_key write license (a Lost-type invalidation
        /// means the resolved command may describe nothing).
        std::uint64_t epoch = 0;
    };

    llvm::DenseMap<std::uint32_t, Entry> entries;

    const Entry* find(std::uint32_t path_id) const {
        auto it = entries.find(path_id);
        return it == entries.end() ? nullptr : &it->second;
    }

    /// The document's projection, or null before its first compile.
    std::shared_ptr<const ASTProjection> projection(std::uint32_t path_id) const {
        const auto* entry = find(path_id);
        return entry ? entry->projection : nullptr;
    }

    /// The last compile landed and no invalidation arrived since
    /// (!ast_dirty of the old world).
    bool current(std::uint32_t path_id) const {
        const auto* entry = find(path_id);
        return entry && entry->current;
    }

    /// Whether the projection's index describes the current buffer. The
    /// arbitration key of the index freshness contract (IndexQuery
    /// clauses 3-4): a current-but-indexless projection (fatal error, no
    /// AST) is an honest gap, not a servable index.
    bool index_current(std::uint32_t path_id) const {
        const auto* entry = find(path_id);
        return entry && entry->current && entry->projection && entry->projection->index &&
               entry->projection->index->loaded();
    }

    std::uint64_t epoch(std::uint32_t path_id) const {
        const auto* entry = find(path_id);
        return entry ? entry->epoch : 0;
    }

    /// Replace one field of the projection, keeping the rest (readers
    /// holding the old shared_ptr are unaffected).
    void set_pch_key(std::uint32_t path_id, std::optional<std::string> pch_key) {
        auto& entry = entries[path_id];
        auto next = entry.projection ? ASTProjection(*entry.projection) : ASTProjection();
        next.pch_key = std::move(pch_key);
        entry.projection = std::make_shared<const ASTProjection>(std::move(next));
    }

    void set_output(std::uint32_t path_id, CompileOutput output) {
        auto& entry = entries[path_id];
        auto next = entry.projection ? ASTProjection(*entry.projection) : ASTProjection();
        next.output = std::move(output);
        entry.projection = std::make_shared<const ASTProjection>(std::move(next));
    }
};

}  // namespace clice

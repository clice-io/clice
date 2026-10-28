module;

#include "support/prelude.h"

module clice:server.document_compile;

import :feature.diagnostics;
import :index.tu_index;
import :project.project;
import :syntax.source_text;
import :vfs.file_table;

namespace clice {

/// The publishable products of the most recent compilation (materialized
/// whole-document feature results). The data lives in the document's compile; the
/// AST family's on_output signal only wakes the push path up — a missed
/// signal is harmless, and with no transport connected the output simply
/// stays put.
struct CompileOutput {
    /// Document version the compile ran against; empty on the clear path
    /// (a failed compile publishes empty diagnostics without a version).
    std::optional<int> version;

    /// The text compiled, which the diagnostics are at bytes of.
    std::shared_ptr<SourceText> text;

    /// What the compile raised and what clice knows of how it compiled.
    feature::CompileDiagnostics diagnostics;

    /// The compile failed before parsing: its diagnostics say why, and
    /// nothing else came of it.
    bool failed = false;
};

/// One open document's compilation products, owned by the AST family and
/// read by everything that used to read them off the Session. Immutable
/// once published: every change installs a fresh one (copy-on-write
/// over the cheap members), so a reader that copied the shared_ptr may
/// keep using it across suspension points.
struct DocumentCompile {
    /// Content key into Project.pch_cache for the PCH the document's parse
    /// loads, if any: its preamble's, else the precompiled header the build
    /// declares for its command. The PCH itself is owned by Project
    /// (shared, content-addressed); whether its preamble-derived state
    /// still describes the buffer is checked against the blob's stored
    /// preamble text at the point of use.
    std::optional<std::string> pch_key;

    /// Content key into Project.pch_cache for the precompiled header the
    /// build declares for the document's command, when the parse loads it:
    /// under its preamble's PCH, or as pch_key itself.
    std::optional<std::string> base_pch_key;

    /// The preamble key whose build failed for the latest compilation,
    /// which then compiled without one: what a save of the document
    /// retries.
    std::optional<std::string> failed_pch_key;

    /// The latest compilation's TU index; null until a compile
    /// commits index data. NOT merged into Project.index — that
    /// only gets disk-derived data from background indexing.
    std::shared_ptr<index::TUIndex> index;

    /// Publishable products of the latest compilation, read by the
    /// transports (see CompileOutput).
    std::optional<CompileOutput> output;

    /// The focus file's entries within `index` (an empty shard when the
    /// compile produced none). Callers gate on index_current, which
    /// implies a loaded TU index.
    const index::Shard& file_shard() const {
        return index->shard_of(index->path_count() - 1);
    }
};

/// The AST family's per-document table: the latest compiles plus the freshness
/// state that used to live on the Session (ast_dirty, dirty_epoch). Owned
/// and written by the family and its facade; readers (IndexQuery,
/// Requests, transports) only look things up. A separate type so
/// read-side tests can populate one without the family's machinery.
struct DocumentCompiles {
    struct Entry {
        std::shared_ptr<const DocumentCompile> latest;

        /// Dependency snapshot from the last successful AST compilation,
        /// used for staleness detection. Kept out of the immutable
        /// compile: no reader outside the family consumes it.
        std::optional<DepsSnapshot> deps;

        /// Whether the latest compile describes the current buffer: the last
        /// compile finished and no invalidation arrived since. The
        /// ast_dirty flag of the old world, inverted, with its single
        /// write points in the family (round commit and invalidation).
        bool current = false;

        /// Bumped by every invalidation of this document. The stateless
        /// build path snapshots it before its PCH acquisition and may
        /// adopt the key only if it is unchanged — the dirty_epoch half
        /// of the retired pch_key write license (a Lost-type invalidation
        /// means the resolved command may describe nothing).
        std::uint64_t epoch = 0;
    };

    llvm::DenseMap<Fid, Entry> entries;

    const Entry* find(Fid path_id) const {
        auto it = entries.find(path_id);
        return it == entries.end() ? nullptr : &it->second;
    }

    /// The document's latest compile, or null before its first compile.
    std::shared_ptr<const DocumentCompile> latest(Fid path_id) const {
        const auto* entry = find(path_id);
        return entry ? entry->latest : nullptr;
    }

    /// The document's latest compile while its output describes buffer
    /// `version`, or null.
    std::shared_ptr<const DocumentCompile> latest_at(Fid path_id, int version) const {
        auto compiled = this->latest(path_id);
        return compiled && compiled->output && compiled->output->version == version ? compiled
                                                                                    : nullptr;
    }

    /// The last compile finished and no invalidation arrived since
    /// (!ast_dirty of the old world).
    bool current(Fid path_id) const {
        const auto* entry = find(path_id);
        return entry && entry->current;
    }

    /// Whether the latest compile's index describes the current buffer. The
    /// arbitration key of the index freshness contract (IndexQuery
    /// clauses 3-4): a current-but-indexless compile (no AST, or an
    /// index the worker could not hand over) is an honest gap, not a
    /// servable index.
    bool index_current(Fid path_id) const {
        const auto* entry = find(path_id);
        return entry && entry->current && entry->latest && entry->latest->index &&
               entry->latest->index->loaded();
    }

    std::uint64_t epoch(Fid path_id) const {
        const auto* entry = find(path_id);
        return entry ? entry->epoch : 0;
    }

    /// Replace some fields of the latest compile, keeping the rest (readers
    /// holding the old shared_ptr are unaffected).
    void set_pch_keys(Fid path_id,
                      std::optional<std::string> pch_key,
                      std::optional<std::string> base_pch_key) {
        auto& entry = entries[path_id];
        auto next = entry.latest ? DocumentCompile(*entry.latest) : DocumentCompile();
        next.pch_key = std::move(pch_key);
        next.base_pch_key = std::move(base_pch_key);
        entry.latest = std::make_shared<const DocumentCompile>(std::move(next));
    }

    void set_output(Fid path_id, CompileOutput output) {
        auto& entry = entries[path_id];
        auto next = entry.latest ? DocumentCompile(*entry.latest) : DocumentCompile();
        next.output = std::move(output);
        entry.latest = std::make_shared<const DocumentCompile>(std::move(next));
    }
};

}  // namespace clice

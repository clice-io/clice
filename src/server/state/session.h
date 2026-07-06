#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "index/tu_index.h"
#include "server/state/workspace.h"

#include "kota/async/async.h"
#include "kota/codec/visit/common.h"
#include "kota/ipc/lsp/position.h"
#include "llvm/ADT/SmallVector.h"

namespace clice {

/// Defined in server/compiler/context_resolver.h — the resolver reports where
/// the compile command came from; Session only stores the verdict.
enum class CommandSource : std::uint8_t;

/// The publishable products of the most recent compilation (materialized
/// whole-document feature results). The data lives here; the compiler's
/// on_output signal only wakes the push path up — a missed signal is
/// harmless, and with no transport connected the output simply stays put.
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

    /// Final inactive regions (PCH preamble + AST merged, byte offsets).
    /// Empty optional on failure: no inactive-regions notification is due.
    std::optional<std::vector<std::uint32_t>> inactive_regions;
};

/// An editing session for a single file opened in the editor.
///
/// Design principle: open files are never depended upon by other files.
/// Dependencies always point to disk files.  The only path from Session
/// to Workspace is didSave, which tells Workspace to rescan the disk file.
///
/// Created on didOpen, destroyed on didClose.  All fields are local to this
/// file's translation unit and NEVER leak to Workspace or other Sessions.
/// Sessions may READ from Workspace (e.g. to obtain PCH/PCM paths, module
/// mappings, include graph) but all compilation results stay here.
struct Session {
    /// Path ID of this file in PathPool.  Set on creation, never changes.
    std::uint32_t path_id = 0;

    /// LSP document version, incremented by the client on each edit.
    int version = 0;

    /// Current buffer content (may differ from disk until saved).
    std::string text;

    /// Byte offsets of each line start in `text`, built by `build_line_starts`.
    /// Updated on didOpen and after every didChange.
    std::vector<std::uint32_t> line_starts;

    /// Construct a LineMap borrowing from this session's text and line_starts.
    kota::ipc::lsp::LineMap line_map() const {
        return kota::ipc::lsp::LineMap(text, line_starts);
    }

    /// Monotonic generation counter, incremented on every didChange and on close.
    /// Used to detect stale compilation results (ABA prevention).
    std::uint64_t generation = 0;

    /// Whether the AST needs to be rebuilt before serving queries.
    bool ast_dirty = true;

    /// Invalidation epoch: bumped every time an event dispatch applies an
    /// AST-invalidating effect to this session (dependency changed on disk,
    /// worker crash, document eviction, ...). A compile snapshots it at
    /// takeoff and may clear ast_dirty on landing only if it is unchanged —
    /// see settle_compile(). Division of labor with generation: generation
    /// answers "is the buffer still the same buffer?", dirty_epoch answers
    /// "did the world get dirty again after I took off?".
    std::uint64_t dirty_epoch = 0;

    /// Clearing ast_dirty is a conditional write — the only sanctioned way
    /// for a compile to declare its product fresh. `launch_epoch` is the
    /// dirty_epoch snapshotted when the compile took off; if any
    /// invalidation landed while it was in flight, the flag stays set and
    /// the next request recompiles. The compile's artifacts (deps snapshot,
    /// file index, diagnostics) may still be recorded — they are not wrong,
    /// only not-current.
    void settle_compile(std::uint64_t launch_epoch) {
        if(dirty_epoch == launch_epoch) {
            ast_dirty = false;
        }
    }

    /// Non-null while a compilation is in flight for this file.
    /// Other queries wait on the event; the compilation task itself
    /// runs independently and cannot be cancelled by LSP $/cancelRequest.
    struct PendingCompile {
        kota::event done;
        bool succeeded = false;

        /// Generation snapshot at spawn; a later didChange supersedes this compile.
        std::uint64_t generation = 0;

        /// True once module dependencies are settled and the compile has moved
        /// on to the worker phase. Past this point the compile holds no
        /// interest in the module graph and superseding it gains nothing —
        /// stale waiters coalesce on its completion instead.
        bool deps_done = false;

        /// Cancels the module-dependency wait when this compile is superseded,
        /// releasing its interest in the old dependency set.
        kota::cancellation_source deps_scope;
    };

    std::shared_ptr<PendingCompile> compiling;

    /// Reference to the PCH entry in Workspace.pch_cache, if any.
    /// The PCH itself is owned by Workspace (shared, content-addressed);
    /// Session only stores enough to locate and validate it.
    struct PCHRef {
        std::string key;          ///< Content key into Workspace.pch_cache.
        std::uint32_t bound = 0;  ///< Preamble byte boundary.
    };

    std::optional<PCHRef> pch_ref;

    /// Dependency snapshot from the last successful AST compilation.
    /// Used for two-layer staleness detection (mtime + content hash).
    std::optional<DepsSnapshot> ast_deps;

    /// Whether this session's self-containment trial has settled. Reset
    /// when compile inputs change for reasons other than buffer edits
    /// (didSave cascades, chain invalidation, mtime staleness), so the
    /// verdict re-evaluates on dependency changes but ordinary typing
    /// errors never trigger a pointless prefix synthesis.
    bool trial_done = false;

    /// Symbol index built from the latest compilation of this file's buffer.
    /// Used for queries (hover, goto, references) on this file.
    /// NOT merged into Workspace.project_index — that only gets disk-derived
    /// data from background indexing.
    std::optional<index::FileIndex> file_index;

    /// Symbol table from the latest compilation, mapping symbol hashes to
    /// names and kinds.
    std::optional<index::SymbolTable> symbols;

    /// Publishable products of the latest compilation, kept for the
    /// transport push path (see CompileOutput).
    std::optional<CompileOutput> output;
};

}  // namespace clice

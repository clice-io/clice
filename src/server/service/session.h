#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "index/tu_index.h"
#include "server/workspace/workspace.h"

#include "kota/async/async.h"
#include "kota/ipc/lsp/position.h"
#include "llvm/ADT/SmallVector.h"

namespace clice {

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
        std::uint32_t path_id = 0;  ///< Key into Workspace.pch_cache.
        std::string key;            ///< CacheStore key at build time.
        std::uint32_t bound = 0;    ///< Preamble byte boundary.
    };

    std::optional<PCHRef> pch_ref;

    /// Dependency snapshot from the last successful AST compilation.
    /// Used for two-layer staleness detection (mtime + content hash).
    std::optional<DepsSnapshot> ast_deps;

    /// Compilation context for header files that lack their own CDB entry.
    /// Stores the host source file and synthesized preamble for this header.
    std::optional<HeaderFileContext> header_context;

    /// User-selected compilation context override (via clice/switchContext).
    /// When set, overrides automatic header context resolution.
    std::optional<std::uint32_t> active_context;

    /// Symbol index built from the latest compilation of this file's buffer.
    /// Used for queries (hover, goto, references) on this file.
    /// NOT merged into Workspace.project_index — that only gets disk-derived
    /// data from background indexing.
    std::optional<index::FileIndex> file_index;

    /// Symbol table from the latest compilation, mapping symbol hashes to
    /// names and kinds.
    std::optional<index::SymbolTable> symbols;
};

}  // namespace clice

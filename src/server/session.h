#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "server/workspace.h"

#include "kota/async/async.h"
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

    /// Monotonic generation counter, incremented on every didChange.
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
    };

    std::shared_ptr<PendingCompile> compiling;

    /// Reference to the PCH entry in Workspace.pch_cache, if any.
    /// The PCH itself is owned by Workspace (shared, content-addressed);
    /// Session only stores enough to locate and validate it.
    struct PCHRef {
        std::uint32_t path_id = 0;  ///< Key into Workspace.pch_cache.
        std::uint64_t hash = 0;     ///< Preamble hash at build time.
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
    std::optional<OpenFileIndex> file_index;
};

}  // namespace clice

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "server/state/quarantine.h"

#include "kota/ipc/lsp/position.h"

namespace clice {

/// How open files are served — the parsed form of the `readonly` config
/// option. Routing is not governed by this: every request is answered by
/// the best source available at that moment (see FeatureRouter); the mode
/// only decides whether PCH/AST builds are a goal at all. Builds are
/// always pull-driven — no lifecycle event starts one, the first request
/// that needs the AST does.
enum class ReadonlyMode : std::uint8_t {
    /// Every open file targets a full AST; the index answers while the
    /// pulled compile is in flight.
    Off,
    /// Never build a PCH: reads serve from the index alone (a cold file
    /// jumps the indexing queue), while completion and signature help
    /// still compile on demand — without a preamble. The agent /
    /// low-resource profile.
    On,
    /// Files start as On and switch to Off at the first edit intent
    /// (edit, completion, signature help, a context switch, a restored
    /// buffer that diverged from the index). A file the index can never
    /// serve — indexing disabled, or its boost attempt settled without a
    /// servable shard — falls back to Off rather than answering empty
    /// forever.
    Auto,
};

/// A session's resource-investment state. Written at exactly two points:
/// session creation (from the readonly mode) and ASTFamily::escalate (the
/// triggers). Everything else derives routing from readiness, not from
/// this flag.
enum class ServingMode : std::uint8_t {
    /// No PCH/AST investment: the session is served from the index.
    IndexOnly,
    /// PCH/AST investment is on; the index still answers while a compile
    /// is in flight.
    Escalated,
};

/// An editing session for a single file opened in the editor.
///
/// Design principle: open files are never depended upon by other files.
/// Dependencies always point to disk files.  The only path from Session
/// to Workspace is didSave, which tells Workspace to rescan the disk file.
///
/// Created on didOpen, destroyed on didClose.  The session holds the
/// buffer and its identity; the document's compilation products live in
/// the AST family's projection (see server/state/ast_projection.h) and
/// NEVER leak to Workspace or other Sessions.
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

    /// Crash containment for this document's content: the crash budget
    /// lives on pool slots, but the poison lives in documents — without
    /// the cut one document burns slot after slot until the whole pool is
    /// dead. All transitions go through the type; see quarantine.h.
    Quarantine quarantine;

    /// See ServingMode for the write discipline. Escalated is the
    /// default so a session constructed outside the didOpen path (tests,
    /// fixtures) behaves like the pre-policy server.
    ServingMode serving = ServingMode::Escalated;

    /// Set when an index projection answered a request for this session;
    /// the compile-output push path reads it to tell clients to re-pull
    /// what the AST now answers better (semantic tokens, inlay hints).
    bool index_served = false;

    /// Whether this session's self-containment trial has settled. Reset
    /// when compile inputs change for reasons other than buffer edits
    /// (didSave cascades, chain invalidation, mtime staleness), so the
    /// verdict re-evaluates on dependency changes but ordinary typing
    /// errors never trigger a pointless prefix synthesis.
    bool trial_done = false;
};

}  // namespace clice

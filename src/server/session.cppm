module;

#include "support/prelude.h"

module clice:server.session;

import :server.quarantine;
import :syntax.position;
import :syntax.source_text;
import :vfs.file_table;

namespace clice {

/// How open files are served — the parsed form of the `readonly` config
/// option. Routing is not governed by this: every request is answered by
/// the best source available at that moment (see Features); the mode
/// only decides whether PCH/AST builds are a goal at all. Builds are
/// always pull-driven — no lifecycle event starts one, the first request
/// that needs the AST does.
enum class ReadonlyMode : std::uint8_t {
    /// Every open file targets a full AST; the index answers while the
    /// pulled compile is in flight.
    Off,
    /// Never build a PCH: reads serve from the index alone (a cold file
    /// jumps the indexing queue), while completion and signature help
    /// still compile on demand — without a preamble. The
    /// low-resource profile.
    On,
    /// Files start as On and switch to Off at the first edit intent
    /// (edit, completion, signature help, a context switch, a restored
    /// buffer that diverged from the index). A file the index can never
    /// serve — indexing disabled, or its boost attempt finished without a
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
/// to Project is didSave, which tells Project to rescan the disk file.
///
/// Created on didOpen, destroyed on didClose.  The session holds the
/// buffer and its identity; the document's compilation products live in
/// the AST family's latest compile (see server/document_compile.cppm) and
/// NEVER leak to Project or other Sessions.
struct Session {
    /// Path ID of this file in FileTable.  Set on creation, never changes.
    Fid path_id;

    /// LSP document version, incremented by the client on each edit.
    int version = 0;

    /// Current buffer content (may differ from disk until saved) and what
    /// is derived from it, read in the dialect Features::text_of sets.
    /// Behind a pointer: a SourceText stays where it was made, its text
    /// pointing into itself, an alias document's session moves, and a
    /// compile's output keeps the text it compiled.
    std::shared_ptr<SourceText> source = std::make_shared<SourceText>(std::string());

    /// xxh3 of the text, rewritten with it: what "the buffer holds these
    /// bytes" is compared by against disk observations and index shards.
    std::uint64_t hash = 0;

    llvm::StringRef text() const {
        return source->text();
    }

    PositionMap positions() const {
        return source->positions();
    }

    /// Replace the text, and with it everything derived from it.
    void set_text(std::string text) {
        source = std::make_shared<SourceText>(std::move(text));
        hash = llvm::xxh3_64bits(source->text());
    }

    /// Monotonic generation counter, incremented on every didChange and on close.
    /// Used to detect stale compilation results (ABA prevention).
    std::uint64_t generation = 0;

    /// What this document's worker crashes bar it from, and when it may
    /// try again. All transitions go through the type; see server/quarantine.cppm.
    /// Shared with the store's parked table across a close, so work still
    /// in flight on a closed session books into the records a reopen
    /// restores.
    std::shared_ptr<Quarantine> quarantine = std::make_shared<Quarantine>();

    /// The store closed or replaced this session: nothing of it is
    /// published any more, its crash notes included.
    bool closed = false;

    /// See ServingMode for the write discipline. Escalated is the
    /// default so a session constructed outside the didOpen path (tests,
    /// fixtures) behaves like the pre-policy server.
    ServingMode serving = ServingMode::Escalated;

    /// Set when a result computed from the index answered a request for this session;
    /// the compile-output push path reads it to tell clients to re-pull
    /// what the AST now answers better (semantic tokens, inlay hints).
    bool index_served = false;

    /// Whether this session's self-containment trial has finished. Reset
    /// when compile inputs change for reasons other than buffer edits
    /// (didSave cascades, chain invalidation, mtime staleness), so the
    /// result re-evaluates on dependency changes but ordinary typing
    /// errors never trigger a pointless prefix synthesis.
    bool trial_done = false;

    /// The PCH pair a crash of this document's compile was put on: the
    /// retraction rebuilds it, and a crash on it again is the document's
    /// own. Cleared by a compile that finishes.
    std::string crashed_pch;

    /// A worker death under this document's compile with clang-tidy on,
    /// held while the compile retries without the pass: finishing blames
    /// the pass, another crash the compile.
    /// FIXME: held on the session, the suspicion is lost when the document
    /// closes during the retry (its next open crashes once more), and a retry
    /// finishing on edited text books the crash there: the pass waits one more edit.
    std::optional<ipc::Error> tidy_crash;
};

/// A request's hold on the buffer it was asked about: the generation
/// snapshot taken as the request is dispatched.
/// Every later decision — adopting a compile product, committing a worker
/// reply, answering at all — asks `fresh()` first; a didChange or
/// didClose bumped the generation, and whatever the request computed
/// describes a buffer that no longer exists. Completion still answers
/// after edits at or past its cursor: its ranges still hold, and the
/// client filters it by what was typed meanwhile.
struct Ticket {
    std::shared_ptr<Session> session;
    std::uint64_t generation = 0;

    /// An empty ticket for no session: a document not open.
    static Ticket take(std::shared_ptr<Session> session) {
        auto generation = session ? session->generation : 0;
        return {std::move(session), generation};
    }

    bool fresh() const {
        return session->generation == generation;
    }
};

}  // namespace clice

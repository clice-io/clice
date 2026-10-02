#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "project/project.h"
#include "sched/families/pch.h"
#include "sched/families/pcm.h"
#include "sched/graph.h"
#include "server/ast_projection.h"
#include "server/session.h"
#include "server/session_store.h"
#include "support/signal.h"
#include "worker/pool.h"

#include "kota/async/async.h"

namespace clice {

namespace testing {

struct ASTFamilyFixture;

}

struct EditorContext;

/// Open documents' ASTs as a task-graph family: one node per document,
/// candidate/durable edges to the PCM and PCH nodes its rounds wait on,
/// one round = one compile (up to two worker sends for the
/// self-containment trial). Assembled server-side: the closure captures
/// the session store, crash quarantine and publishing — state the sched
/// layer must not see.
///
/// The family owns the whole compile lifecycle the retired Compiler ran:
/// dependency preparation (through RoundContext::depend, the only wait
/// form that records edges), the stateful dispatch, crash accounting,
/// trial planning, and the projection landing. Contract 16: the landed
/// package (projection, deps snapshot, serving bits, output push) is
/// written inside the round with no suspension before the outcome becomes
/// visible — a joiner must never see Success before the projection.
///
/// Supersede and invalidation flow through the facade's single write
/// points; the round distinguishes them at landing: a Lost invalidation
/// (buffer unchanged) still publishes its products as bounded staleness
/// and reports Stale, a content supersede discards wholesale.
class ASTFamily {
public:
    ASTFamily(Project& project,
              EditorContext& contexts,
              TaskGraph& graph,
              PCMFamily& pcm,
              PCHFamily& pch,
              WorkerPool& pool,
              SessionStore& sessions,
              kota::event_loop& loop);

    /// Register the production runner. Tests that drive the facade
    /// against a synthetic runner register their own under Family::AST.
    void register_runner();

    /// Per-document projections and freshness state; the read surface for
    /// IndexQuery, Features and the transports.
    ASTProjectionTable projections;

    /// Parsed form of config.project.readonly, written once by the master
    /// at initialization.
    ReadonlyMode readonly = ReadonlyMode::Off;

    /// Compile an open file's AST if it is not current: join the
    /// document's round (spawning one if none is live) and retry through
    /// stale attempts until a terminal outcome. Returns true when the
    /// projection is current on return. A buffer edit or session
    /// replacement abandons the join — the editor re-requests after an
    /// edit, so the result would describe a buffer that no longer exists.
    kota::task<bool> ensure_compiled(std::shared_ptr<Session> session);

    /// Start (or join) the session's compile detached — the caller keeps
    /// serving from the index while it lands. The graph's one-round-per-
    /// node discipline dedupes concurrent kicks.
    void request_compile(std::shared_ptr<Session> session);

    /// The escalation triggers' single write point: flip an index-only
    /// session to Escalated. The build stays pull-driven — the next
    /// request that needs the AST starts it. A no-op for
    /// already-escalated sessions and under readonly = "on" (the mode
    /// transition is disabled).
    void escalate(Session& session);

    /// The inputs of a stateless build carrying the session's buffer:
    /// module dependencies built or revalidated, the preamble PCH built or
    /// reused, and the PCM path table. Under readonly = "on" the PCH step
    /// is skipped — the build compiles without a preamble, the profile's
    /// stated trade.
    struct StatelessInputs {
        std::pair<std::string, std::uint32_t> pch;
        std::unordered_map<std::string, std::string> pcms;
    };

    /// `ticket` is the pch_key write license together with the projection
    /// epoch snapshotted here, before the first suspension: a supersede
    /// bumps the generation, a Lost-type invalidation bumps only the
    /// epoch — either way the resolved command may describe a command
    /// that no longer exists, and adopting the key would hand later
    /// incomplete-preamble edits a stale-flag PCH. This path runs no
    /// graph round it could ask instead. The inputs themselves follow
    /// `text`, the buffer the build sends, so a request that outlives its
    /// ticket still compiles against a PCH of its own preamble.
    kota::task<bool> prepare_stateless_inputs(const Ticket& ticket,
                                              llvm::StringRef text,
                                              const std::string& directory,
                                              const std::vector<std::string>& arguments,
                                              const Resolution& resolution,
                                              StatelessInputs& inputs);

    /// The edit path's whole supersede (didChange): the buffer moved, so
    /// the projection is no longer current and the in-flight round's
    /// result is void. Fires the round's advisory token and interrupts
    /// the worker's parse with a CancelCompile notification — FIFO order
    /// puts it ahead of any replacement Compile, and the round still
    /// observes its real reply (crash accounting depends on it).
    void supersede(Fid path_id);

    /// A Lost-type invalidation (dependency changed on disk, worker
    /// crash, eviction — the buffer itself is unchanged): the projection
    /// is no longer current and an in-flight round must not land as
    /// such, but its parse keeps running — the round publishes the
    /// product as bounded staleness before reporting Stale.
    void invalidate(Fid path_id);

    /// didClose (and didOpen replacing a live session): the document's
    /// products die with it.
    void drop(Fid path_id);

    /// The files the document's compile depends on: the ones is_stale()
    /// checks, and the inputs of the modules it imports.
    void closure(Fid path_id, llvm::SmallVectorImpl<Fid>& files);

    /// clice/switchContext: the new context is a different compilation
    /// identity. Supersede any in-flight compile and drop the state
    /// earned under the old one — including the self-containment trial,
    /// since a different host can change the macro environment.
    void switch_identity(Session& session);

    /// Emitted after a compile round (or an announcement path)
    /// materializes publishable products into the document's projection.
    /// Subscribers read the output from the projection; with no
    /// subscriber connected the output simply stays there.
    Signal<std::shared_ptr<Session>> on_output;

    /// Callback invoked when indexing should be scheduled.
    std::function<void()> on_indexing_needed;

    /// Invoked from ensure_compiled's fast path when the pull-side
    /// staleness check finds an input of the document changed on disk. The
    /// owner invalidates the document itself (synchronously); the changed
    /// file's own cascade comes from the file table, whose look during the
    /// check queued the change like any other.
    std::function<void(Fid path_id)> on_stale;

    /// A dispatch of `kind` killed a worker on the document's content —
    /// `error` carries the death (worker::death_of, the cause in the
    /// message). Records it in the session's quarantine and republishes, so
    /// the crash note shows.
    void record_crash(const std::shared_ptr<Session>& session,
                      std::uint8_t kind,
                      const kota::ipc::Error& error);

    /// Push the document's output again: its crash notes changed with no
    /// compile to carry them. A document that never compiled gets an empty
    /// versionless output to hang them on.
    void republish(const std::shared_ptr<Session>& session);

    /// The compile, or the preamble it consumes, crashed a worker and has
    /// no license to try again: the document has no AST to offer.
    static bool compile_barred(const Session& session);

    /// Whether a round of the document is live: it will send a compile of
    /// its own, so a lost worker-side AST needs no invalidation.
    bool compiling(Fid path_id) const {
        return graph.is_compiling(node(path_id));
    }

    /// didSave: every crashed kind of the document retries on its next
    /// request, the artifacts it consumes included.
    void saved(Session& session);

    /// Install `output` as the document's current output and wake the
    /// push path (the didClose diagnostics retraction).
    void publish_output(const std::shared_ptr<Session>& session, CompileOutput output);

    /// Interrupt every in-flight parse and wait for the detached compile
    /// joins. The rounds themselves land in TaskGraph::shutdown — the
    /// interruption is what keeps that landing prompt (a round's stateful
    /// send deliberately carries no advisory token; contract 2 wants the
    /// real reply, and CancelCompile makes it arrive early).
    kota::task<> stop();

private:
    static NodeId node(Fid path_id) {
        return {Family::AST, path_id.raw};
    }

    /// One compile round; see the class comment for its obligations.
    kota::task<RoundOutcome> run(RoundContext& ctx, Fid path_id);

    /// The module-dependency phase of a round: resolve imports from the
    /// round's buffer snapshot under the round's resolved command,
    /// revalidate on-disk PCM blobs, declare the Ast→PCM durable edges
    /// (scanner truth — they must survive a failed compile or fixing an
    /// import could never re-dirty this document), and wait on each
    /// import through depend. False when cancelled: an import whose build
    /// failed is left to the parse, which reports it on the import next to
    /// the file's own diagnostics; one whose build crashed a worker also
    /// lands in the session's quarantine, and is refused until the session
    /// holds a license to retry it.
    kota::task<bool> depend_modules(RoundContext& ctx,
                                    const std::shared_ptr<Session>& session,
                                    const Resolution& resolution,
                                    llvm::StringRef directory,
                                    const std::vector<std::string>& arguments,
                                    llvm::StringRef text);

    /// Non-const: the check observes the disk through the file table.
    bool is_stale(const Session& session);

    /// What a buffer state owes the PCH family: nothing (an empty
    /// preamble with no injected prefix — a previously adopted key must
    /// be cleared), a deferral (the preamble is mid-edit and nothing
    /// fresh exists under its key: keep `previous`, the last adopted key,
    /// while its artifact is still built), or the acquisition of
    /// `request`.
    struct PCHPlan {
        enum class Verdict : std::uint8_t { None, Defer, Acquire };

        Verdict verdict = Verdict::None;
        PCHFamily::Request request;
        std::optional<std::string> previous;
    };

    PCHPlan plan_pch(Fid path_id,
                     llvm::StringRef text,
                     const std::string& directory,
                     const std::vector<std::string>& arguments,
                     const SynthesizedContext* synthesized);

    /// Revalidate or build the preamble PCH of `text` through the family
    /// and adopt its key under the request's license (see
    /// prepare_stateless_inputs). Returns the key the request compiles
    /// against, adopted or not; none when it compiles without a PCH.
    kota::task<std::optional<std::string>> ensure_pch(const std::shared_ptr<Session>& session,
                                                      llvm::StringRef text,
                                                      std::uint64_t license_generation,
                                                      std::uint64_t license_epoch,
                                                      const std::string& directory,
                                                      const std::vector<std::string>& arguments,
                                                      const SynthesizedContext* synthesized);

    friend struct testing::ASTFamilyFixture;

    /// current=false + epoch bump: the shared prefix of every
    /// invalidation flavor.
    void touch(Fid path_id);

    Project& project;
    EditorContext& contexts;
    TaskGraph& graph;
    PCMFamily& pcm;
    PCHFamily& pch;
    WorkerPool& pool;
    SessionStore& sessions;

    /// Detached ensure_compiled joins from request_compile; the rounds
    /// live in the graph's task group.
    kota::task_group<> kicks;
};

/// Discriminators for Quarantine's per-kind records; query kinds follow
/// past Count.
enum class EvidenceKind : std::uint8_t {
    Compile,
    PCH,
    PCM,
    DocumentLink,
    FoldingRange,
    CodeAction,
    Completion,
    SignatureHelp,
    Format,
    Count,
};

constexpr std::uint8_t evidence_kind(EvidenceKind kind) {
    return static_cast<std::uint8_t>(kind);
}

constexpr std::uint8_t evidence_kind(worker::QueryKind kind) {
    return static_cast<std::uint8_t>(EvidenceKind::Count) + static_cast<std::uint8_t>(kind);
}

/// The diagnostics telling what of the document is paused by crashes, and
/// how it comes back; appended to every publish of the document.
void append_crash_notes(const Session& session, std::vector<protocol::Diagnostic>& diagnostics);

}  // namespace clice

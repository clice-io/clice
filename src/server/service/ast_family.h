#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "sched/families/pch.h"
#include "sched/families/pcm.h"
#include "sched/graph.h"
#include "sched/workspace.h"
#include "server/state/ast_projection.h"
#include "server/state/session.h"
#include "server/state/session_store.h"
#include "support/signal.h"
#include "worker/pool.h"

#include "kota/async/async.h"

namespace clice {

namespace testing {

struct ASTFixture;

}

class ContextResolver;

/// The AST family's id in the task graph. Node keys are document path_ids
/// widened into NodeId::key.
constexpr inline std::uint8_t ast_family = 3;

/// Open documents' ASTs as a task-graph family: one node per document,
/// candidate/durable edges to the PCM and PCH nodes its rounds wait on,
/// one round = one compile (up to two worker sends for the
/// self-containment trial). Assembled server-side: the closure captures
/// the session store, quarantine and publishing — state the sched layer
/// must not see.
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
    ASTFamily(Workspace& workspace,
              ContextResolver& contexts,
              TaskGraph& graph,
              PCMFamily& pcm,
              PCHFamily& pch,
              WorkerPool& pool,
              SessionStore& sessions,
              kota::event_loop& loop);

    /// Register the production runner. Tests that drive the facade
    /// against a synthetic runner register their own under ast_family.
    void register_runner();

    /// Per-document projections and freshness state; the read surface for
    /// IndexQuery, FeatureRouter and the transports.
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

    /// The edit path's whole supersede (didChange): the buffer moved, so
    /// the projection is no longer current and the in-flight round's
    /// result is void. Fires the round's advisory token and interrupts
    /// the worker's parse with a CancelCompile notification — FIFO order
    /// puts it ahead of any replacement Compile, and the round still
    /// observes its real reply (crash accounting depends on it).
    void supersede(std::uint32_t path_id);

    /// A Lost-type invalidation (dependency changed on disk, worker
    /// crash, eviction — the buffer itself is unchanged): the projection
    /// is no longer current and an in-flight round must not land as
    /// such, but its parse keeps running — the round publishes the
    /// product as bounded staleness before reporting Stale.
    void invalidate(std::uint32_t path_id);

    /// didClose (and didOpen replacing a live session): the document's
    /// products die with it.
    void drop(std::uint32_t path_id);

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
    /// staleness check finds a dependency changed on disk. The owner routes
    /// it into the event pipeline as a DiskChanged (synchronously), so lazy
    /// detection and the file tracker's polling share one invalidation
    /// cascade instead of maintaining two.
    std::function<void(std::uint32_t path_id)> on_stale;

    /// Publish the quarantine diagnostic as the document's current output
    /// and mark the spell announced. `source` falls back to the previous
    /// output's command source when the announcement has no compile of its
    /// own (the ensure_compiled entry gate).
    void publish_quarantined(const std::shared_ptr<Session>& session,
                             std::optional<CommandSource> source,
                             std::optional<std::uint32_t> line_limit);

    /// Clear the published quarantine diagnostic after a stateless or
    /// query recovery lifted the quarantine: no compile ran to overwrite
    /// the output, and the stale "file is quarantined" must not linger.
    void publish_recovered(const std::shared_ptr<Session>& session);

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
    static NodeId node(std::uint32_t path_id) {
        return {ast_family, path_id};
    }

    /// One compile round; see the class comment for its obligations.
    kota::task<RoundOutcome> run(RoundContext& ctx, std::uint32_t path_id);

    /// The module-dependency phase of a round: resolve imports from the
    /// round's buffer snapshot under the round's resolved command,
    /// revalidate on-disk PCM blobs, declare the Ast→PCM durable edges
    /// (scanner truth — they must survive a failed compile or fixing an
    /// import could never re-dirty this document), and wait on each
    /// import through depend.
    kota::task<DependResult> depend_modules(RoundContext& ctx,
                                            std::uint32_t path_id,
                                            llvm::StringRef directory,
                                            const std::vector<std::string>& arguments,
                                            llvm::StringRef text);

    /// Non-const: a passing staleness check may repair the snapshots'
    /// stat fast paths in place (see deps_changed).
    bool is_stale(const Session& session);

    /// current=false + epoch bump: the shared prefix of every
    /// invalidation flavor.
    void touch(std::uint32_t path_id);

    Workspace& workspace;
    ContextResolver& contexts;
    TaskGraph& graph;
    PCMFamily& pcm;
    PCHFamily& pch;
    WorkerPool& pool;
    SessionStore& sessions;

    /// Detached ensure_compiled joins from request_compile; the rounds
    /// live in the graph's task group.
    kota::task_group<> kicks;

    friend struct testing::ASTFixture;
};

/// The PCH acquisition plan of a buffer state: whether a PCH is owed at
/// all, and the request that identifies/builds it when so. Shared by the
/// AST round (depend path) and the stateless forward path (acquire path).
struct PCHPlan {
    /// False when the preamble is empty and no prefix is injected — a
    /// PCH would be empty, and a previously adopted key must be cleared.
    bool wanted = false;
    PCHFamily::Request request;
};

PCHPlan plan_pch(Workspace& workspace,
                 ContextResolver& contexts,
                 std::uint32_t path_id,
                 llvm::StringRef text,
                 const std::string& directory,
                 const std::vector<std::string>& arguments);

/// Evidence-kind discriminators for Quarantine's per-kind ledgers. Queries
/// and stateless builds share one space, offset so they cannot collide;
/// document links have no QueryKind and get their own slot. The stateless
/// slots keep the values of the retired BuildKind enum (0x40 + kind) —
/// they are in-memory only, but drift within a session would misattribute
/// evidence.
constexpr inline std::uint8_t document_link_evidence = 0x20;
constexpr inline std::uint8_t pch_evidence = 0x40;
constexpr inline std::uint8_t completion_evidence = 0x43;
constexpr inline std::uint8_t signature_help_evidence = 0x44;
constexpr inline std::uint8_t format_evidence = 0x45;

}  // namespace clice

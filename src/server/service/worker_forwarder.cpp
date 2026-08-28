#include "server/service/worker_forwarder.h"

#include <utility>

#include "sched/context.h"
#include "server/protocol/position.h"
#include "support/anomaly.h"
#include "support/logging.h"
#include "support/timer.h"
#include "syntax/scan.h"
#include "worker/protocol.h"

#include "kota/ipc/lsp/position.h"
#include "kota/ipc/lsp/uri.h"

namespace clice {

namespace lsp = kota::ipc::lsp;
using serde_raw = kota::codec::RawValue;

/// Send a stateless request, resending once if the worker died mid-request.
/// The pool does not retry on its own — it marks the dead slot and surfaces
/// worker_crashed, so the resend lands on a healthy worker. Build tasks are
/// idempotent; one retry suffices, since a request that kills two workers in
/// a row is a poison workload that a third attempt would not survive either.
///
/// `on_crash` fires once per attempt that killed a worker — evidence is
/// counted per death, not per request, so a poison build that burns two
/// workers spends two strikes. Callers must count ONLY through it: the
/// returned error is the retry's status, which may not be a crash.
template <typename Params, typename OnCrash>
static kota::ipc::RequestResult<Params>
    send_stateless_retrying(WorkerPool& pool,
                            Params params,
                            worker::Priority priority,
                            OnCrash on_crash,
                            kota::ipc::request_options opts = {}) {
    auto result = co_await pool.send_stateless(params, priority, opts);
    if(!result.has_value() && result.error().code == worker::dispatch_errc::worker_crashed) {
        on_crash(result.error());
        result = co_await pool.send_stateless(params, priority, opts);
        if(!result.has_value() && result.error().code == worker::dispatch_errc::worker_crashed) {
            on_crash(result.error());
        }
    }
    co_return std::move(result);
}

/// Every stateless build carrying an open document's content goes through
/// here: each worker kill is blamed on the session's ledger for `kind`
/// before the caller sees the result, so no site can forget the
/// accounting. Clearing the kind on success stays with the caller — it
/// must be guarded by the launch generation, or a stale reply would
/// launder evidence the new content recorded meanwhile. Grep for
/// build_for to enumerate every such site.
template <typename Params>
static kota::ipc::RequestResult<Params> build_for(WorkerPool& pool,
                                                  Session& session,
                                                  std::uint8_t kind,
                                                  Params params,
                                                  worker::Priority priority,
                                                  kota::ipc::request_options opts) {
    return send_stateless_retrying(
        pool,
        std::move(params),
        priority,
        [&session, kind](const kota::ipc::protocol::Error& error) {
            session.quarantine.on_kind_crash(kind, worker::death_of(error));
        },
        opts);
}

constexpr static std::uint8_t evidence_kind(worker::QueryKind kind) {
    return static_cast<std::uint8_t>(EvidenceKind::Count) + static_cast<std::uint8_t>(kind);
}

WorkerForwarder::WorkerForwarder(Workspace& workspace,
                                 ContextResolver& contexts,
                                 PCMFamily& pcm,
                                 PCHFamily& pch,
                                 ASTFamily& ast,
                                 WorkerPool& pool) :
    workspace(workspace), contexts(contexts), pcm(pcm), pch(pch), ast(ast), pool(pool) {}

kota::task<bool> WorkerForwarder::ensure_pch(const std::shared_ptr<Session>& session,
                                             std::uint64_t license_generation,
                                             std::uint64_t license_epoch,
                                             const std::string& directory,
                                             const std::vector<std::string>& arguments) {
    auto path_id = session->path_id;
    auto license = [&] {
        return session->generation == license_generation &&
               ast.projections.epoch(path_id) == license_epoch;
    };
    // A request invalidated during its earlier awaits (module
    // dependencies) must not touch the adopted key at all: the reset
    // branch below writes it before the first suspension point.
    if(!license()) {
        co_return false;
    }

    auto plan = plan_pch(workspace, contexts, path_id, session->text, directory, arguments);
    if(!plan.wanted) {
        ast.projections.set_pch_key(path_id, std::nullopt);
        co_return true;
    }
    auto pch_key = plan.request.pch_key;

    // Preamble incomplete (user still typing) and nothing fresh to adopt
    // under the new key — defer the rebuild, keep using the previously
    // adopted PCH if it is still available.
    if(!pch.fresh(pch_key) && !is_preamble_complete(session->text, plan.request.preamble_bound)) {
        LOG_DEBUG("Preamble incomplete for {}, deferring PCH rebuild",
                  workspace.path_pool.resolve(path_id));
        auto projection = ast.projections.projection(path_id);
        if(projection && projection->pch_key.has_value()) {
            auto it = workspace.pch_cache.find(*projection->pch_key);
            co_return it != workspace.pch_cache.end() && !it->second.path.empty();
        }
        co_return false;
    }

    // Revalidate or build through the family. This request is the
    // dispatch owner when its acquire spawns the round; the probe then
    // pins every worker death of the build on this document (the preamble
    // is its content), held by the round so the evidence lands even if
    // this request goes stale meanwhile — a stale request's crashes still
    // count. Joiners of an already-running round install nothing and
    // replay nothing.
    auto outcome = co_await pch.acquire(std::move(plan.request), [session](llvm::StringRef death) {
        session->quarantine.on_kind_crash(evidence_kind(EvidenceKind::PCH), death);
    });
    if(outcome != PCHFamily::Outcome::Ready) {
        co_return false;
    }

    // Adoption is gated on the round outcome, never on leftover cache
    // paths, and on this request's own license: a supersede or a
    // Lost-type invalidation while we waited means the resolved command
    // may describe nothing — neither the key write nor the evidence
    // wash belongs to this request anymore.
    if(!license()) {
        co_return false;
    }
    ast.projections.set_pch_key(path_id, pch_key);
    // Adopting a proven-good artifact disproves the session's PCH strikes
    // as surely as building one — but only its own; every consumer washes
    // for itself.
    session->quarantine.on_kind_land(evidence_kind(EvidenceKind::PCH));
    co_return true;
}

kota::task<bool>
    WorkerForwarder::prepare_inputs(const std::shared_ptr<Session>& session,
                                    std::uint64_t license_generation,
                                    std::uint64_t license_epoch,
                                    const std::string& directory,
                                    const std::vector<std::string>& arguments,
                                    std::pair<std::string, uint32_t>& pch_pair,
                                    std::unordered_map<std::string, std::string>& pcms) {
    auto path_id = session->path_id;

    // A user request waits on these builds: dispatch them High so the
    // background budget cannot throttle its own foreground. The scan
    // runs under the request's command with the same text the dispatch
    // will compile — buffer plus any appended suffix include — so an
    // unsaved `import m;` (or one inside a contextual header's suffix)
    // builds its PCM before the parse needs it.
    llvm::SmallVector<const char*, 32> argv;
    argv.reserve(arguments.size());
    for(auto& arg: arguments) {
        argv.push_back(arg.c_str());
    }
    auto scan_text = session->text;
    contexts.append_suffix_include(path_id, scan_text);
    if(!co_await pcm.prepare_deps(path_id,
                                  argv,
                                  directory,
                                  std::optional<llvm::StringRef>(scan_text),
                                  /*foreground=*/true)) {
        co_return false;
    }

    // Build or reuse PCH. Under readonly = "on" the build compiles
    // without a preamble instead — completion and signature help pay full
    // parses, the profile's stated trade.
    if(ast.readonly != ReadonlyMode::On) {
        auto pch_ok =
            co_await ensure_pch(session, license_generation, license_epoch, directory, arguments);
        auto projection = ast.projections.projection(path_id);
        if(pch_ok && projection && projection->pch_key.has_value()) {
            if(auto pch_it = workspace.pch_cache.find(*projection->pch_key);
               pch_it != workspace.pch_cache.end()) {
                pch_pair = {pch_it->second.path, pch_it->second.bound};
            }
        }
    }

    // Fill all available PCM paths, excluding the file's own PCM
    // to avoid "multiple module declarations".
    workspace.fill_pcm_deps(pcms, path_id);

    co_return true;
}

WorkerForwarder::RawResult
    WorkerForwarder::forward_query(worker::QueryKind kind,
                                   std::shared_ptr<Session> session,
                                   std::optional<protocol::Position> position,
                                   std::optional<protocol::Range> range,
                                   std::optional<kota::cancellation_token> token) {
    auto path_id = session->path_id;
    auto path = std::string(workspace.path_pool.resolve(path_id));
    auto gen = session->generation;
    auto map = session->line_map();

    ScopedTimer timer;
    if(!co_await ast.ensure_compiled(session)) {
        co_return serde_raw{"null"};
    }
    auto wait_ms = timer.ms_f();

    if(session->generation != gen) {
        co_return serde_raw{"null"};
    }

    worker::QueryParams wp;
    wp.kind = kind;
    wp.path = path;
    wp.config = workspace.config;

    if(position) {
        wp.offset = clamped_offset(map, *position);
    }

    if(range) {
        wp.range = {clamped_offset(map, range->start), clamped_offset(map, range->end)};
        if(wp.range.begin > wp.range.end) {
            co_return kota::outcome_error(
                kota::ipc::Error{kota::ipc::protocol::ErrorCode::InvalidParams,
                                 "Range start is after its end"});
        }
    }

    // This kind holding strikes without a probe is neither licensed
    // recovery nor safe ordinary work.
    if(session->quarantine.kind_blocked(evidence_kind(kind))) {
        co_return kota::outcome_error(
            kota::ipc::Error{worker::dispatch_errc::worker_unavailable, "Document is quarantined"});
    }

    // A recovery query — this kind holds the strikes — is still
    // distrusted: it needs the owner (the AST lives there), but its crash
    // spends no slot budget and new documents avoid the worker while it
    // flies. The guard spends the probe the edit licensed (a harmless kind
    // must not: hover would strand a semantic-tokens quarantine) and hands
    // it back unless the attempt recorded a strike.
    bool recovery = session->quarantine.recovery_kind(evidence_kind(kind));
    auto suspect = recovery ? Suspect::InPlace : Suspect::No;
    std::optional<Quarantine::ProbeGuard> probe_guard;
    if(recovery) {
        probe_guard.emplace(session->quarantine);
    }
    auto result = co_await pool.send_stateful(path_id, wp, {.token = std::move(token)}, suspect);
    if(!result.has_value()) {
        // A query that kills the worker is this document's doing even
        // though its compile landed: per-kind ledger, since only this query
        // kind answering disproves it (see Quarantine::on_kind_crash).
        if(result.error().code == worker::dispatch_errc::worker_crashed) {
            session->quarantine.on_kind_crash(evidence_kind(kind),
                                              worker::death_of(result.error()));
        }
        if(!worker::is_operational_error(result.error())) {
            LOG_ANOMALY(WorkerRequestFail,
                        "query (kind={}) failed for {}: {}",
                        kind,
                        path,
                        result.error().message);
        }
        co_return kota::outcome_error(std::move(result.error()));
    }
    // The reply proves queries on the DISPATCHED content answer; an edit
    // that landed mid-flight must not launder the new content's ledger —
    // crashes count regardless of staleness, successes only when fresh.
    if(session->generation == gen) {
        bool was_active = session->quarantine.active();
        session->quarantine.on_kind_land(evidence_kind(kind));
        if(was_active && !session->quarantine.active()) {
            ast.publish_recovered(session);
        }
    }
    LOG_PERF("request",
             "kind={} file={} wait_ms={:.2f} total_ms={:.2f}",
             kind,
             path,
             wait_ms,
             timer.ms_f());
    co_return std::move(result.value());
}

kota::task<std::vector<feature::DocumentLink>, kota::ipc::Error>
    WorkerForwarder::forward_document_links(std::shared_ptr<Session> session,
                                            std::optional<kota::cancellation_token> token) {
    auto path_id = session->path_id;
    auto path = std::string(workspace.path_pool.resolve(path_id));
    auto gen = session->generation;

    ScopedTimer timer;
    if(!co_await ast.ensure_compiled(session)) {
        co_return std::vector<feature::DocumentLink>{};
    }
    if(session->generation != gen) {
        co_return std::vector<feature::DocumentLink>{};
    }
    auto wait_ms = timer.ms_f();

    if(session->quarantine.kind_blocked(evidence_kind(EvidenceKind::DocumentLink))) {
        co_return kota::outcome_error(
            kota::ipc::Error{worker::dispatch_errc::worker_unavailable, "Document is quarantined"});
    }

    bool recovery = session->quarantine.recovery_kind(evidence_kind(EvidenceKind::DocumentLink));
    auto suspect = recovery ? Suspect::InPlace : Suspect::No;
    std::optional<Quarantine::ProbeGuard> probe_guard;
    if(recovery) {
        probe_guard.emplace(session->quarantine);
    }
    auto result = co_await pool.send_stateful(path_id,
                                              worker::DocumentLinkParams{path},
                                              {.token = std::move(token)},
                                              suspect);
    if(!result.has_value()) {
        if(result.error().code == worker::dispatch_errc::worker_crashed) {
            session->quarantine.on_kind_crash(evidence_kind(EvidenceKind::DocumentLink),
                                              worker::death_of(result.error()));
        }
        if(!worker::is_operational_error(result.error())) {
            LOG_ANOMALY(WorkerRequestFail,
                        "documentLink failed for {}: {}",
                        path,
                        result.error().message);
        }
        co_return kota::outcome_error(std::move(result.error()));
    }
    // The result carries byte offsets against the compiled buffer; a
    // didChange that landed during the await makes them describe text the
    // session no longer holds — the reply edge would map them onto the
    // edited buffer at wrong positions. The same staleness gates the query
    // ledger: a stale success must not launder the new content's evidence.
    if(session->generation != gen) {
        co_return std::vector<feature::DocumentLink>{};
    }
    bool was_active = session->quarantine.active();
    session->quarantine.on_kind_land(evidence_kind(EvidenceKind::DocumentLink));
    if(was_active && !session->quarantine.active()) {
        ast.publish_recovered(session);
    }
    LOG_PERF("request",
             "kind=DocumentLink file={} wait_ms={:.2f} total_ms={:.2f}",
             path,
             wait_ms,
             timer.ms_f());
    co_return std::move(result.value());
}

template <typename Params>
WorkerForwarder::RawResult
    WorkerForwarder::forward_interactive(std::uint8_t evidence,
                                         llvm::StringRef label,
                                         protocol::Position position,
                                         std::shared_ptr<Session> session,
                                         std::optional<kota::cancellation_token> token) {
    auto path_id = session->path_id;
    auto path = std::string(workspace.path_pool.resolve(path_id));
    auto gen = session->generation;

    // This build compiles the same content the quarantine watches: while
    // the document is quarantined, only the recovery dispatch — the kind
    // holding the strikes, with the probe armed — may run. Anything else
    // is arbitrary work on proven-poisonous content. A refusal announces
    // the quarantine, or a completion-only client would never see it.
    if(session->quarantine.active() && !session->quarantine.recovery_kind(evidence)) {
        LOG_WARN("forward_interactive: {} is quarantined, refusing build", path);
        if(session->quarantine.needs_announcement()) {
            ast.publish_quarantined(session, std::nullopt, std::nullopt);
        }
        co_return kota::outcome_error(
            kota::ipc::Error{worker::dispatch_errc::worker_unavailable, "Document is quarantined"});
    }
    auto flight = session->quarantine.begin_flight();

    // Takeoff snapshot for the pch_key write license (see prepare_inputs):
    // this request runs concurrently with compiles and holds no round, so
    // it is the easiest continuation to come back stale after a disk/CDB
    // change.
    auto epoch = ast.projections.epoch(path_id);

    Params wp;
    wp.file = path;
    wp.text = session->text;
    contexts.resolve_command(path, wp.directory, wp.arguments, ContextUse::Editor);
    contexts.append_suffix_include(path_id, wp.text);
    wp.config = workspace.config;

    ScopedTimer timer;
    if(!co_await prepare_inputs(session, gen, epoch, wp.directory, wp.arguments, wp.pch, wp.pcms)) {
        LOG_WARN("forward_interactive: dependency preparation failed for {}", path);
        co_return kota::outcome_error(kota::ipc::Error{"Dependency preparation failed"});
    }
    // A PCH crash inside the preparation may have tipped the document into
    // quarantine after the entry gate: stop before dispatching the same
    // content again — that crash also spent any armed probe (it WAS the
    // attempt). A probe that predates this request's own evidence does not
    // excuse dispatching content that just proved poisonous.
    if(session->quarantine.active() && session->quarantine.grew(flight)) {
        session->quarantine.spend_probe();
        LOG_WARN("forward_interactive: {} quarantined during dependency prep", path);
        if(session->quarantine.needs_announcement()) {
            ast.publish_quarantined(session, std::nullopt, std::nullopt);
        }
        co_return kota::outcome_error(
            kota::ipc::Error{worker::dispatch_errc::worker_unavailable, "Document is quarantined"});
    }
    auto wait_ms = timer.ms_f();

    if(session->generation != gen) {
        co_return serde_raw{"null"};
    }

    lsp::LineMap map(wp.text);
    wp.offset = clamped_offset(map, position);

    // The recovery license is re-taken here: the gate's answer may have
    // been spent by a concurrent recovery during the deps await. The guard
    // holds the spent probe across the dispatch and hands it back if the
    // coroutine unwinds (cancellation) or fails before any attempt ran —
    // an unavailable retry after a crashed first attempt keeps it spent,
    // the crash was the licensed attempt.
    bool recovery = session->quarantine.recovery_kind(evidence);
    if(session->quarantine.active() && !recovery) {
        co_return kota::outcome_error(
            kota::ipc::Error{worker::dispatch_errc::worker_unavailable, "Document is quarantined"});
    }
    std::optional<Quarantine::ProbeGuard> probe_guard;
    if(recovery) {
        probe_guard.emplace(session->quarantine);
    }
    auto result = co_await build_for(pool,
                                     *session,
                                     evidence,
                                     wp,
                                     worker::Priority::High,
                                     {.token = std::move(token)});
    if(!result.has_value()) {
        if(!worker::is_operational_error(result.error())) {
            LOG_ANOMALY(WorkerRequestFail,
                        "build (kind={}) failed for {}: {}",
                        label,
                        path,
                        result.error().message);
        }
        co_return kota::outcome_error(std::move(result.error()));
    }
    // The reply proves this kind on the DISPATCHED content answers; a
    // stale success must not launder evidence the new content recorded.
    // Leaving quarantine here clears the published diagnostic — no compile
    // runs to overwrite it.
    if(session->generation == gen) {
        bool was_active = session->quarantine.active();
        session->quarantine.on_kind_land(evidence);
        if(was_active && !session->quarantine.active()) {
            ast.publish_recovered(session);
        }
    }
    LOG_PERF("request",
             "kind={} file={} wait_ms={:.2f} total_ms={:.2f}",
             label,
             path,
             wait_ms,
             timer.ms_f());
    co_return std::move(result.value());
}

WorkerForwarder::RawResult
    WorkerForwarder::forward_completion(const protocol::Position& position,
                                        std::shared_ptr<Session> session,
                                        std::optional<kota::cancellation_token> token) {
    return forward_interactive<worker::CompletionParams>(evidence_kind(EvidenceKind::Completion),
                                                         "Completion",
                                                         position,
                                                         std::move(session),
                                                         std::move(token));
}

WorkerForwarder::RawResult
    WorkerForwarder::forward_signature_help(const protocol::Position& position,
                                            std::shared_ptr<Session> session,
                                            std::optional<kota::cancellation_token> token) {
    return forward_interactive<worker::SignatureHelpParams>(
        evidence_kind(EvidenceKind::SignatureHelp),
        "SignatureHelp",
        position,
        std::move(session),
        std::move(token));
}

WorkerForwarder::RawResult
    WorkerForwarder::forward_format(std::shared_ptr<Session> session,
                                    std::optional<protocol::Range> range,
                                    std::optional<kota::cancellation_token> token) {
    auto path_id = session->path_id;
    auto path = std::string(workspace.path_pool.resolve(path_id));
    auto gen = session->generation;

    // Formatting runs no sema, but it is still this document's content on
    // a worker: while quarantined, only format-as-recovery may run, and a
    // refusal announces the quarantine.
    bool recovery = session->quarantine.recovery_kind(evidence_kind(EvidenceKind::Format));
    if(session->quarantine.active() && !recovery) {
        LOG_WARN("forward_format: {} is quarantined, refusing format", path);
        if(session->quarantine.needs_announcement()) {
            ast.publish_quarantined(session, std::nullopt, std::nullopt);
        }
        co_return kota::outcome_error(
            kota::ipc::Error{worker::dispatch_errc::worker_unavailable, "Document is quarantined"});
    }

    worker::FormatParams wp;
    wp.file = path;
    wp.text = session->text;

    if(range) {
        lsp::LineMap map(wp.text);
        wp.range = {clamped_offset(map, range->start), clamped_offset(map, range->end)};
        if(wp.range.begin > wp.range.end) {
            co_return kota::outcome_error(
                kota::ipc::Error{kota::ipc::protocol::ErrorCode::InvalidParams,
                                 "Range start is after its end"});
        }
    }

    ScopedTimer timer;
    std::optional<Quarantine::ProbeGuard> probe_guard;
    if(recovery) {
        probe_guard.emplace(session->quarantine);
    }
    auto result = co_await build_for(pool,
                                     *session,
                                     evidence_kind(EvidenceKind::Format),
                                     wp,
                                     worker::Priority::High,
                                     {.token = std::move(token)});
    if(!result.has_value()) {
        if(!worker::is_operational_error(result.error())) {
            LOG_ANOMALY(WorkerRequestFail,
                        "format failed for {}: {}",
                        path,
                        result.error().message);
        }
        co_return kota::outcome_error(std::move(result.error()));
    }
    if(session->generation == gen) {
        bool was_active = session->quarantine.active();
        session->quarantine.on_kind_land(evidence_kind(EvidenceKind::Format));
        if(was_active && !session->quarantine.active()) {
            ast.publish_recovered(session);
        }
    }
    LOG_PERF("request", "kind=Format file={} total_ms={:.2f}", path, timer.ms_f());
    co_return std::move(result.value());
}

}  // namespace clice

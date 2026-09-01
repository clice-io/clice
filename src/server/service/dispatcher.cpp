#include "server/service/dispatcher.h"

#include <utility>

#include "sched/context.h"
#include "server/protocol/position.h"
#include "support/anomaly.h"
#include "support/logging.h"
#include "support/timer.h"
#include "worker/protocol.h"

#include "kota/ipc/lsp/position.h"
#include "kota/meta/enum.h"

namespace clice {

namespace lsp = kota::ipc::lsp;
using serde_raw = kota::codec::RawValue;

kota::ipc::Error content_modified() {
    return kota::ipc::Error{content_modified_code,
                            "Document changed while the request was in flight"};
}

namespace {

kota::ipc::Error quarantined() {
    return kota::ipc::Error{worker::dispatch_errc::worker_unavailable, "Document is quarantined"};
}

constexpr std::uint8_t evidence_kind(worker::QueryKind kind) {
    return static_cast<std::uint8_t>(EvidenceKind::Count) + static_cast<std::uint8_t>(kind);
}

/// One request's passage through the document's quarantine: the gate at
/// entry, and the recovery license armed right before dispatch. A
/// quarantined document's recovery dispatch — the kind holding the
/// strikes, with the edit-granted probe armed — is still distrusted: its
/// crash spends no slot budget and new documents avoid the worker while it
/// flies. The guard spends the probe and hands it back unless the attempt
/// recorded a strike, so a cancelled recovery does not strand the document
/// until another edit.
class QuarantineGate {
public:
    enum class Scope : std::uint8_t {
        /// A query against the settled AST: only strikes of this very kind
        /// bar it — the compile that produced the AST is the recovery for
        /// compile strikes, and a harmless hover must not spend a
        /// semantic-tokens quarantine's probe.
        Kind,
        /// A build of the buffer's content: any active quarantine refuses
        /// it unless this kind is the one holding the strikes — anything
        /// else is arbitrary work on proven-poisonous content.
        Content,
    };

    QuarantineGate(Quarantine& quarantine, std::uint8_t kind, Scope scope) :
        quarantine(quarantine), recovery(quarantine.recovery_kind(kind)),
        refuse(scope == Scope::Kind ? quarantine.kind_blocked(kind)
                                    : quarantine.active() && !recovery) {}

    QuarantineGate(const QuarantineGate&) = delete;
    QuarantineGate& operator=(const QuarantineGate&) = delete;

    bool refused() const {
        return refuse;
    }

    /// Spend the probe for the dispatch; the guard holds it across the
    /// send and returns it if the coroutine unwinds before any attempt
    /// dispatched — an unavailable retry after a crashed first attempt
    /// keeps it spent, the crash was the licensed attempt.
    void arm() {
        if(recovery) {
            guard.emplace(quarantine);
        }
    }

    Suspect suspect() const {
        return recovery ? Suspect::InPlace : Suspect::No;
    }

private:
    Quarantine& quarantine;
    bool recovery;
    bool refuse;
    std::optional<Quarantine::ProbeGuard> guard;
};

}  // namespace

Dispatcher::Dispatcher(Workspace& workspace,
                       ContextResolver& contexts,
                       ASTFamily& ast,
                       WorkerPool& pool) :
    workspace(workspace), contexts(contexts), ast(ast), pool(pool) {}

kota::ipc::Error Dispatcher::refuse(const std::shared_ptr<Session>& session) {
    if(session->quarantine.needs_announcement()) {
        ast.publish_quarantined(session, std::nullopt, std::nullopt);
    }
    return quarantined();
}

template <typename Outcome>
Outcome Dispatcher::land(const Ticket& ticket,
                         std::uint8_t kind,
                         llvm::StringRef label,
                         Outcome result) {
    auto& session = *ticket.session;
    if(!result.has_value()) {
        if(!worker::is_operational_error(result.error())) {
            LOG_ANOMALY(WorkerRequestFail,
                        "{} failed for {}: {}",
                        label,
                        workspace.file_table.resolve(session.path_id),
                        result.error().message);
        }
        return result;
    }
    // The reply proves this kind on the DISPATCHED content answers; an edit
    // that landed mid-flight must not launder the new content's ledger —
    // crashes were counted per attempt regardless of staleness, a success
    // settles only when fresh. Leaving quarantine here clears the published
    // diagnostic: no compile runs to overwrite it.
    if(!ticket.fresh()) {
        return Outcome{kota::outcome_error(content_modified())};
    }
    bool was_active = session.quarantine.active();
    session.quarantine.on_kind_land(kind);
    if(was_active && !session.quarantine.active()) {
        ast.publish_recovered(ticket.session);
    }
    return result;
}

Dispatcher::RawResult Dispatcher::query(worker::QueryKind kind,
                                        const Ticket& ticket,
                                        std::optional<protocol::Position> position,
                                        std::optional<protocol::Range> range,
                                        std::optional<kota::cancellation_token> token) {
    auto& session = *ticket.session;
    auto path_id = session.path_id;
    auto path = std::string(workspace.file_table.resolve(path_id));
    auto evidence = evidence_kind(kind);
    auto label = kota::meta::enum_name(kind, "Unknown");

    ScopedTimer timer;
    if(!co_await ast.ensure_compiled(ticket.session)) {
        // The join abandons on an edit or ends without an AST (failed
        // compile, quarantine): only the former is the client's cue to
        // re-pull; the latter is the honest "no AST" answer.
        if(!ticket.fresh()) {
            co_return kota::outcome_error(content_modified());
        }
        co_return serde_raw{"null"};
    }
    if(!ticket.fresh()) {
        co_return kota::outcome_error(content_modified());
    }
    auto wait_ms = timer.ms_f();

    worker::QueryParams wp;
    wp.kind = kind;
    wp.path = path;
    wp.config = workspace.config;

    auto map = session.line_map();
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

    QuarantineGate gate(session.quarantine, evidence, QuarantineGate::Scope::Kind);
    if(gate.refused()) {
        co_return kota::outcome_error(quarantined());
    }
    gate.arm();
    auto result =
        co_await pool.send_stateful(path_id.raw, wp, {.token = std::move(token)}, gate.suspect());
    // A query that kills the worker is this document's doing even though
    // its compile landed: per-kind ledger, since only this query kind
    // answering disproves it (see Quarantine::on_kind_crash).
    if(!result.has_value() && result.error().code == worker::dispatch_errc::worker_crashed) {
        session.quarantine.on_kind_crash(evidence, worker::death_of(result.error()));
    }
    result = land(ticket, evidence, label, std::move(result));
    if(result.has_value()) {
        LOG_PERF("request",
                 "kind={} file={} wait_ms={:.2f} total_ms={:.2f}",
                 label,
                 path,
                 wait_ms,
                 timer.ms_f());
    }
    co_return std::move(result);
}

kota::task<std::vector<feature::DocumentLink>, kota::ipc::Error>
    Dispatcher::document_links(const Ticket& ticket,
                               std::optional<kota::cancellation_token> token) {
    auto& session = *ticket.session;
    auto path_id = session.path_id;
    auto path = std::string(workspace.file_table.resolve(path_id));
    auto evidence = evidence_kind(EvidenceKind::DocumentLink);

    ScopedTimer timer;
    if(!co_await ast.ensure_compiled(ticket.session)) {
        if(!ticket.fresh()) {
            co_return kota::outcome_error(content_modified());
        }
        co_return std::vector<feature::DocumentLink>{};
    }
    if(!ticket.fresh()) {
        co_return kota::outcome_error(content_modified());
    }
    auto wait_ms = timer.ms_f();

    QuarantineGate gate(session.quarantine, evidence, QuarantineGate::Scope::Kind);
    if(gate.refused()) {
        co_return kota::outcome_error(quarantined());
    }
    gate.arm();
    auto result = co_await pool.send_stateful(path_id.raw,
                                              worker::DocumentLinkParams{path},
                                              {.token = std::move(token)},
                                              gate.suspect());
    if(!result.has_value() && result.error().code == worker::dispatch_errc::worker_crashed) {
        session.quarantine.on_kind_crash(evidence, worker::death_of(result.error()));
    }
    result = land(ticket, evidence, "DocumentLink", std::move(result));
    if(result.has_value()) {
        LOG_PERF("request",
                 "kind=DocumentLink file={} wait_ms={:.2f} total_ms={:.2f}",
                 path,
                 wait_ms,
                 timer.ms_f());
    }
    co_return std::move(result);
}

template <typename Params>
Dispatcher::RawResult Dispatcher::interactive(std::uint8_t evidence,
                                              llvm::StringRef label,
                                              const Ticket& ticket,
                                              protocol::Position position,
                                              std::optional<kota::cancellation_token> token) {
    auto& session = *ticket.session;
    auto path_id = session.path_id;
    auto path = std::string(workspace.file_table.resolve(path_id));

    // This build compiles the same content the quarantine watches.
    QuarantineGate entry(session.quarantine, evidence, QuarantineGate::Scope::Content);
    if(entry.refused()) {
        LOG_WARN("{}: {} is quarantined, refusing build", label, path);
        co_return kota::outcome_error(refuse(ticket.session));
    }
    auto flight = session.quarantine.begin_flight();

    Params wp;
    wp.file = path;
    wp.text = session.text;
    contexts.resolve_command(path, wp.directory, wp.arguments, ContextUse::Editor);
    contexts.append_suffix_include(path_id, wp.text);
    wp.config = workspace.config;

    ScopedTimer timer;
    ASTFamily::StatelessInputs inputs;
    if(!co_await ast.prepare_stateless_inputs(ticket, wp.directory, wp.arguments, inputs)) {
        LOG_WARN("{}: dependency preparation failed for {}", label, path);
        co_return kota::outcome_error(kota::ipc::Error{"Dependency preparation failed"});
    }
    wp.pch = std::move(inputs.pch);
    wp.pcms = std::move(inputs.pcms);
    // A PCH crash inside the preparation may have tipped the document into
    // quarantine after the entry gate: stop before dispatching the same
    // content again — that crash also spent any armed probe (it WAS the
    // attempt). A probe that predates this request's own evidence does not
    // excuse dispatching content that just proved poisonous.
    if(session.quarantine.active() && session.quarantine.grew(flight)) {
        session.quarantine.spend_probe();
        LOG_WARN("{}: {} quarantined during dependency prep", label, path);
        co_return kota::outcome_error(refuse(ticket.session));
    }
    auto wait_ms = timer.ms_f();

    if(!ticket.fresh()) {
        co_return kota::outcome_error(content_modified());
    }

    lsp::LineMap map(wp.text);
    wp.offset = clamped_offset(map, position);

    // The license is re-taken here: the entry gate's answer may have been
    // spent by a concurrent recovery during the dependency awaits.
    QuarantineGate license(session.quarantine, evidence, QuarantineGate::Scope::Content);
    if(license.refused()) {
        co_return kota::outcome_error(quarantined());
    }
    license.arm();
    auto result = co_await send_stateless_retrying(
        pool,
        std::move(wp),
        worker::Priority::High,
        [&session, evidence](const kota::ipc::protocol::Error& error) {
            session.quarantine.on_kind_crash(evidence, worker::death_of(error));
        },
        {.token = std::move(token)});
    result = land(ticket, evidence, label, std::move(result));
    if(result.has_value()) {
        LOG_PERF("request",
                 "kind={} file={} wait_ms={:.2f} total_ms={:.2f}",
                 label,
                 path,
                 wait_ms,
                 timer.ms_f());
    }
    co_return std::move(result);
}

Dispatcher::RawResult Dispatcher::completion(const Ticket& ticket,
                                             const protocol::Position& position,
                                             std::optional<kota::cancellation_token> token) {
    return interactive<worker::CompletionParams>(evidence_kind(EvidenceKind::Completion),
                                                 "Completion",
                                                 ticket,
                                                 position,
                                                 std::move(token));
}

Dispatcher::RawResult Dispatcher::signature_help(const Ticket& ticket,
                                                 const protocol::Position& position,
                                                 std::optional<kota::cancellation_token> token) {
    return interactive<worker::SignatureHelpParams>(evidence_kind(EvidenceKind::SignatureHelp),
                                                    "SignatureHelp",
                                                    ticket,
                                                    position,
                                                    std::move(token));
}

Dispatcher::RawResult Dispatcher::format(const Ticket& ticket,
                                         std::optional<protocol::Range> range,
                                         std::optional<kota::cancellation_token> token) {
    auto& session = *ticket.session;
    auto path = std::string(workspace.file_table.resolve(session.path_id));
    auto evidence = evidence_kind(EvidenceKind::Format);

    QuarantineGate gate(session.quarantine, evidence, QuarantineGate::Scope::Content);
    if(gate.refused()) {
        LOG_WARN("Format: {} is quarantined, refusing format", path);
        co_return kota::outcome_error(refuse(ticket.session));
    }

    worker::FormatParams wp;
    wp.file = path;
    wp.text = session.text;

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
    gate.arm();
    auto result = co_await send_stateless_retrying(
        pool,
        std::move(wp),
        worker::Priority::High,
        [&session, evidence](const kota::ipc::protocol::Error& error) {
            session.quarantine.on_kind_crash(evidence, worker::death_of(error));
        },
        {.token = std::move(token)});
    result = land(ticket, evidence, "Format", std::move(result));
    if(result.has_value()) {
        LOG_PERF("request", "kind=Format file={} total_ms={:.2f}", path, timer.ms_f());
    }
    co_return std::move(result);
}

}  // namespace clice

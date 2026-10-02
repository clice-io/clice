#include "server/dispatcher.h"

#include <type_traits>
#include <utility>

#include "server/editor_context.h"
#include "server/position.h"
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

kota::ipc::Error invalid_range() {
    return kota::ipc::Error{kota::ipc::protocol::ErrorCode::InvalidParams,
                            "Range start is after its end"};
}

/// What a request answers when the workers could not serve it.
template <typename Outcome>
Outcome empty_answer() {
    using Value = typename Outcome::value_type;
    if constexpr(std::is_same_v<Value, serde_raw>) {
        return serde_raw{"null"};
    } else {
        return Value{};
    }
}

}  // namespace

Dispatcher::Dispatcher(Project& project,
                       EditorContext& contexts,
                       ASTFamily& ast,
                       WorkerPool& pool) :
    project(project), contexts(contexts), ast(ast), pool(pool) {}

template <typename Params>
RequestResult<Params> Dispatcher::ask(const Ticket& ticket,
                                      const Params& params,
                                      std::optional<kota::cancellation_token> token,
                                      bool compile,
                                      bool& no_ast) {
    for(bool reloaded = false;; reloaded = true) {
        if(compile && !co_await ast.ensure_compiled(ticket.session)) {
            no_ast = true;
            co_return kota::outcome_error(
                kota::ipc::Error{worker::dispatch_errc::cancelled, "No AST to ask"});
        }
        compile = true;
        auto result =
            co_await pool.send_stateful(ticket.session->path_id.raw, params, {.token = token});
        if(reloaded || result.has_value() ||
           result.error().code != worker::dispatch_errc::document_unloaded) {
            co_return std::move(result);
        }
        // The worker evicted the document behind a projection that still
        // reads current: its eviction notice crossed a compile of the
        // document still landing, which the master took for the one that
        // would put it back. Compile it there again.
        ast.invalidate(ticket.session->path_id);
    }
}

template <typename Outcome>
Outcome Dispatcher::land(const Ticket& ticket,
                         std::uint8_t kind,
                         llvm::StringRef label,
                         Outcome result,
                         bool snapshot) {
    auto& session = *ticket.session;
    if(!result.has_value()) {
        if(!worker::is_operational_error(result.error())) {
            LOG_ANOMALY(WorkerRequestFail,
                        "{} failed for {}: {}",
                        label,
                        project.file_table.resolve(session.path_id),
                        result.error().message);
            return result;
        }
        // A client's own cancel stays a cancel.
        if(result.error().code == worker::dispatch_errc::cancelled) {
            return result;
        }
        LOG_INFO("{} for {} answers empty: {}",
                 label,
                 project.file_table.resolve(session.path_id),
                 result.error().message);
        return empty_answer<Outcome>();
    }
    // The reply proves this kind on the DISPATCHED content answers; an edit
    // that landed mid-flight must not clear the new content's record —
    // crashes were counted per attempt regardless of staleness, a success
    // settles only when fresh. Clearing a record republishes: no compile
    // runs to drop its note.
    if(!ticket.fresh()) {
        if(snapshot) {
            return result;
        }
        return Outcome{kota::outcome_error(content_modified())};
    }
    if(session.quarantine.crashed(kind)) {
        session.quarantine.on_land(kind);
        ast.republish(ticket.session);
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
    auto path = std::string(project.file_table.resolve(path_id));
    auto evidence = evidence_kind(kind);
    auto label = kota::meta::enum_name(kind, "Unknown");

    if(session.quarantine.barred(evidence, Quarantine::Clock::now())) {
        co_return serde_raw{"null"};
    }

    ScopedTimer timer;
    if(!co_await ast.ensure_compiled(ticket.session)) {
        // The join abandons on an edit or ends without an AST (failed
        // compile, a crash bar): only the former is the client's cue to
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
    wp.config = project.config;

    auto map = session.line_map();
    if(position) {
        wp.offset = clamped_offset(map, *position);
    }
    if(range) {
        auto clamped = clamped_range(map, *range);
        if(!clamped) {
            co_return kota::outcome_error(invalid_range());
        }
        wp.range = *clamped;
    }

    // A query that kills the worker is this document's doing even though
    // its compile landed: per-kind record, since only this query kind
    // answering clears it (see Quarantine).
    Quarantine::Attempt attempt(session.quarantine, evidence);
    bool no_ast = false;
    bool compile = false;
    auto result = co_await deliver(
        pool,
        true,
        [&] { return ask(ticket, wp, token, std::exchange(compile, true), no_ast); },
        [&](const kota::ipc::Error& error) { ast.record_crash(ticket.session, evidence, error); });
    if(no_ast) {
        if(!ticket.fresh()) {
            co_return kota::outcome_error(content_modified());
        }
        co_return serde_raw{"null"};
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

template <typename Params>
kota::task<typename protocol::RequestTraits<Params>::Result, kota::ipc::Error>
    Dispatcher::typed(const Ticket& ticket,
                      EvidenceKind kind,
                      llvm::StringRef label,
                      Params params,
                      std::optional<kota::cancellation_token> token) {
    using Result = typename protocol::RequestTraits<Params>::Result;
    auto& session = *ticket.session;
    auto path_id = session.path_id;
    auto evidence = evidence_kind(kind);

    if(session.quarantine.barred(evidence, Quarantine::Clock::now())) {
        co_return Result{};
    }

    ScopedTimer timer;
    if(!co_await ast.ensure_compiled(ticket.session)) {
        if(!ticket.fresh()) {
            co_return kota::outcome_error(content_modified());
        }
        co_return Result{};
    }
    if(!ticket.fresh()) {
        co_return kota::outcome_error(content_modified());
    }
    auto wait_ms = timer.ms_f();

    Quarantine::Attempt attempt(session.quarantine, evidence);
    bool no_ast = false;
    bool compile = false;
    auto result = co_await deliver(
        pool,
        true,
        [&] { return ask(ticket, params, token, std::exchange(compile, true), no_ast); },
        [&](const kota::ipc::Error& error) { ast.record_crash(ticket.session, evidence, error); });
    if(no_ast) {
        if(!ticket.fresh()) {
            co_return kota::outcome_error(content_modified());
        }
        co_return Result{};
    }
    result = land(ticket, evidence, label, std::move(result));
    if(result.has_value()) {
        LOG_PERF("request",
                 "kind={} file={} wait_ms={:.2f} total_ms={:.2f}",
                 label,
                 project.file_table.resolve(path_id),
                 wait_ms,
                 timer.ms_f());
    }
    co_return std::move(result);
}

kota::task<std::vector<feature::DocumentLink>, kota::ipc::Error>
    Dispatcher::document_links(const Ticket& ticket,
                               std::optional<kota::cancellation_token> token) {
    auto path = std::string(project.file_table.resolve(ticket.session->path_id));
    co_return co_await typed(ticket,
                             EvidenceKind::DocumentLink,
                             "DocumentLink",
                             worker::DocumentLinkParams{std::move(path)},
                             std::move(token));
}

kota::task<std::optional<std::vector<feature::FoldingRange>>, kota::ipc::Error>
    Dispatcher::folding_ranges(const Ticket& ticket,
                               std::optional<kota::cancellation_token> token) {
    auto path = std::string(project.file_table.resolve(ticket.session->path_id));
    co_return co_await typed(ticket,
                             EvidenceKind::FoldingRange,
                             "FoldingRange",
                             worker::FoldingRangeParams{std::move(path)},
                             std::move(token));
}

kota::task<std::vector<feature::CodeAction>, kota::ipc::Error>
    Dispatcher::code_actions(const Ticket& ticket,
                             const protocol::Range& range,
                             std::optional<kota::cancellation_token> token) {
    // Clamped against the buffer the ticket was taken on: a buffer that
    // moves before the reply lands turns the reply into ContentModified.
    auto selection = clamped_range(ticket.session->line_map(), range);
    if(!selection) {
        co_return kota::outcome_error(invalid_range());
    }
    auto path = std::string(project.file_table.resolve(ticket.session->path_id));
    co_return co_await typed(ticket,
                             EvidenceKind::CodeAction,
                             "CodeAction",
                             worker::CodeActionParams{std::move(path), *selection},
                             std::move(token));
}

template <typename Params>
Dispatcher::RawResult Dispatcher::interactive(std::uint8_t evidence,
                                              llvm::StringRef label,
                                              const Ticket& ticket,
                                              protocol::Position position,
                                              Params wp,
                                              std::optional<kota::cancellation_token> token) {
    auto& session = *ticket.session;
    auto path_id = session.path_id;
    auto path = std::string(project.file_table.resolve(path_id));
    // This build parses the same content as the compile: a compile or
    // preamble that crashed bars it as well as its own crashes do.
    auto barred = [&] {
        return session.quarantine.barred(evidence, Quarantine::Clock::now()) ||
               ASTFamily::compile_barred(session);
    };
    if(barred()) {
        LOG_DEBUG("{}: {} is barred by a crash", label, path);
        co_return serde_raw{"null"};
    }

    wp.file = path;
    wp.text = session.text;
    auto resolution = contexts.resolve_command(path_id, wp.directory, wp.arguments);
    wp.config = project.config;

    ScopedTimer timer;
    ASTFamily::StatelessInputs inputs;
    if(!co_await ast.prepare_stateless_inputs(ticket,
                                              wp.text,
                                              wp.directory,
                                              wp.arguments,
                                              resolution.synthesized.get(),
                                              inputs)) {
        LOG_WARN("{}: dependency preparation failed for {}", label, path);
        co_return kota::outcome_error(kota::ipc::Error{"Dependency preparation failed"});
    }
    wp.pch = std::move(inputs.pch);
    wp.pcms = std::move(inputs.pcms);
    // A preamble crash inside the preparation bars the content: stop
    // before dispatching it again.
    if(barred()) {
        LOG_DEBUG("{}: {} was barred during dependency prep", label, path);
        co_return serde_raw{"null"};
    }
    auto wait_ms = timer.ms_f();

    lsp::LineMap map(wp.text);
    wp.offset = clamped_offset(map, position);

    // A completion reply stays useful after edits at or past the cursor:
    // its ranges still hold, and the client filters it by what was typed
    // meanwhile.
    auto carried = wp.text.substr(0, wp.offset);
    auto snapshot = [&] {
        return std::is_same_v<Params, worker::CompletionParams> &&
               session.text.starts_with(carried);
    };
    if(!ticket.fresh() && !snapshot()) {
        co_return kota::outcome_error(content_modified());
    }

    if(resolution.synthesized) {
        wp.synthesized = resolution.synthesized->files;
        resolution.synthesized->append_suffix_include(wp.text);
    }

    Quarantine::Attempt attempt(session.quarantine, evidence);
    auto result = co_await deliver(
        pool,
        false,
        [&] { return pool.send_stateless(wp, worker::Priority::High, {.token = token}); },
        [&](const kota::ipc::Error& error) { ast.record_crash(ticket.session, evidence, error); });
    result = land(ticket, evidence, label, std::move(result), snapshot());
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
                                             const feature::CompletionClient& client,
                                             std::optional<kota::cancellation_token> token) {
    return interactive(evidence_kind(EvidenceKind::Completion),
                       "Completion",
                       ticket,
                       position,
                       worker::CompletionParams{.client = client},
                       std::move(token));
}

Dispatcher::RawResult Dispatcher::signature_help(const Ticket& ticket,
                                                 const protocol::Position& position,
                                                 std::optional<kota::cancellation_token> token) {
    return interactive(evidence_kind(EvidenceKind::SignatureHelp),
                       "SignatureHelp",
                       ticket,
                       position,
                       worker::SignatureHelpParams{},
                       std::move(token));
}

Dispatcher::RawResult Dispatcher::format(const Ticket& ticket,
                                         std::optional<protocol::Range> range,
                                         std::optional<kota::cancellation_token> token) {
    auto& session = *ticket.session;
    auto path = std::string(project.file_table.resolve(session.path_id));
    auto evidence = evidence_kind(EvidenceKind::Format);

    if(session.quarantine.barred(evidence, Quarantine::Clock::now())) {
        LOG_DEBUG("Format: {} is barred by a crash", path);
        co_return serde_raw{"null"};
    }

    worker::FormatParams wp;
    wp.file = project.file_table.display(session.path_id);
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
    Quarantine::Attempt attempt(session.quarantine, evidence);
    auto result = co_await deliver(
        pool,
        false,
        [&] { return pool.send_stateless(wp, worker::Priority::High, {.token = token}); },
        [&](const kota::ipc::Error& error) { ast.record_crash(ticket.session, evidence, error); });
    result = land(ticket, evidence, "Format", std::move(result));
    if(result.has_value()) {
        LOG_PERF("request", "kind=Format file={} total_ms={:.2f}", path, timer.ms_f());
    }
    co_return std::move(result);
}

}  // namespace clice

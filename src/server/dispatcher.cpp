module;

#include "support/prelude.h"

#include "support/anomaly.macros.h"
#include "support/logging.macros.h"

module clice;

import :feature.code_action.code_action;
import :feature.code_completion;
import :feature.document_symbols;
import :feature.folding_ranges;
import :feature.inlay_hints;
import :feature.semantic_tokens;
import :server.dispatcher;
import :server.editor_context;
import :support.anomaly;
import :support.json;
import :support.logging;
import :support.timer;
import :worker.protocol;

namespace clice {

using serde_raw = codec::RawValue;

ipc::Error content_modified() {
    return ipc::Error{content_modified_code, "Document changed while the request was in flight"};
}

namespace {

/// What a request answers when the workers could not serve it.
template <typename Value>
Value empty_value() {
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
                                      std::uint8_t evidence,
                                      const Params& params,
                                      kota::cancellation_token token,
                                      bool& unanswered) {
    auto unsent = [&] {
        unanswered = true;
        return kota::outcome_error(ipc::Error{worker::dispatch_errc::cancelled, "Query not sent"});
    };
    // The caller's compile gave a crash of the kind time to be booked.
    if(ticket.session->quarantine->barred(evidence, Quarantine::Clock::now())) {
        co_return unsent();
    }
    // A query that kills the worker is this document's doing even though
    // its compile finished: per-kind record, since only this query kind
    // answering clears it (see Quarantine).
    Quarantine::Attempt attempt(*ticket.session->quarantine, evidence);
    bool compile = false;
    co_return co_await deliver(
        pool,
        true,
        [&]() -> RequestResult<Params> {
            while(true) {
                if(std::exchange(compile, true) && !co_await ast.ensure_compiled(ticket.session)) {
                    co_return unsent();
                }
                if(attempt.overtaken()) {
                    co_return unsent();
                }
                auto result = co_await pool.send_stateful(ticket.session->path_id.raw,
                                                          params,
                                                          {.token = token});
                if(result.has_value() ||
                   result.error().code != worker::dispatch_errc::document_unloaded) {
                    co_return std::move(result);
                }
                // The worker evicted the document behind a compile that
                // still reads current: its eviction notice crossed a compile
                // of the document still finishing, which the master took for
                // the one that would put it back, or another document's
                // request arrived between this compile's reply and the query.
                // Compile it there again. Each eviction takes another request
                // arriving in that window, so the retries end with the work in
                // flight.
                ast.invalidate(ticket.session->path_id);
            }
        },
        [&](const ipc::Error& error) { ast.record_crash(ticket.session, evidence, error); });
}

template <typename Outcome>
Outcome Dispatcher::finish(const Ticket& ticket,
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
    }
    // Answered or not, a stale request tells the client to re-pull.
    if(!ticket.fresh() && !snapshot) {
        return Outcome{kota::outcome_error(content_modified())};
    }
    if(!result.has_value()) {
        LOG_INFO("{} for {} answers empty: {}",
                 label,
                 project.file_table.resolve(session.path_id),
                 result.error().message);
        return empty_value<typename Outcome::value_type>();
    }
    // The reply proves this kind on the DISPATCHED content answers; an edit
    // that arrived mid-flight must not clear the new content's record —
    // crashes were counted per attempt regardless of staleness, a success
    // clears only when fresh. Clearing a record republishes: no compile
    // runs to drop its note.
    if(ticket.fresh() && session.quarantine->crashed(kind)) {
        session.quarantine->on_success(kind);
        ast.republish(ticket.session);
    }
    return result;
}

Dispatcher::RawResult Dispatcher::hover(const Ticket& ticket,
                                        std::optional<protocol::Position> position,
                                        kota::cancellation_token token,
                                        protocol::MarkupKind markup) {
    auto& session = *ticket.session;
    auto path_id = session.path_id;
    auto path = std::string(project.file_table.resolve(path_id));
    auto evidence = evidence_kind(EvidenceKind::Hover);
    llvm::StringRef label = "Hover";

    if(session.quarantine->barred(evidence, Quarantine::Clock::now())) {
        co_return serde_raw{"null"};
    }

    ScopedTimer timer;
    if(!co_await ast.ensure_compiled(ticket.session)) {
        // The join abandons on an edit or ends without an AST (failed
        // compile, a crash bar): only the former is the client's cue to
        // re-pull; the latter is the honest "no AST" answer.
        if(!ticket.fresh()) {
            co_await kota::fail(content_modified());
        }
        co_return serde_raw{"null"};
    }
    if(!ticket.fresh()) {
        co_await kota::fail(content_modified());
    }
    auto wait_ms = timer.ms_f();

    worker::HoverParams wp;
    wp.path = path;
    wp.config = project.config;
    wp.markup = markup;

    if(position) {
        wp.offset = session.positions().offset_clamped(*position, PositionEncoding::UTF16);
    }

    bool unanswered = false;
    auto result = co_await ask(ticket, evidence, wp, token, unanswered);
    if(unanswered) {
        if(!ticket.fresh()) {
            co_await kota::fail(content_modified());
        }
        co_return serde_raw{"null"};
    }
    result = finish(ticket, evidence, label, std::move(result));
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
kota::task<typename protocol::RequestTraits<Params>::Result, ipc::Error>
    Dispatcher::typed(const Ticket& ticket,
                      EvidenceKind kind,
                      llvm::StringRef label,
                      Params params,
                      kota::cancellation_token token) {
    using Result = typename protocol::RequestTraits<Params>::Result;
    auto& session = *ticket.session;
    auto path_id = session.path_id;
    auto evidence = evidence_kind(kind);

    if(session.quarantine->barred(evidence, Quarantine::Clock::now())) {
        co_return Result{};
    }

    ScopedTimer timer;
    if(!co_await ast.ensure_compiled(ticket.session)) {
        if(!ticket.fresh()) {
            co_await kota::fail(content_modified());
        }
        co_return Result{};
    }
    if(!ticket.fresh()) {
        co_await kota::fail(content_modified());
    }
    auto wait_ms = timer.ms_f();

    bool unanswered = false;
    auto result = co_await ask(ticket, evidence, params, token, unanswered);
    if(unanswered) {
        if(!ticket.fresh()) {
            co_await kota::fail(content_modified());
        }
        co_return Result{};
    }
    result = finish(ticket, evidence, label, std::move(result));
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

kota::task<std::vector<feature::InlayHint>, ipc::Error>
    Dispatcher::inlay_hints(const Ticket& ticket,
                            const protocol::Range& range,
                            kota::cancellation_token token) {
    // Clamped against the buffer the ticket was taken on: a buffer that
    // moves before the reply arrives turns the reply into ContentModified.
    auto offsets = ticket.session->positions().offset_range(range, PositionEncoding::UTF16);
    auto path = std::string(project.file_table.resolve(ticket.session->path_id));
    co_return co_await typed(ticket,
                             EvidenceKind::InlayHint,
                             "InlayHint",
                             worker::InlayHintParams{std::move(path), offsets, project.config},
                             std::move(token));
}

kota::task<std::vector<index::DocumentLink>, ipc::Error>
    Dispatcher::document_links(const Ticket& ticket, kota::cancellation_token token) {
    auto path = std::string(project.file_table.resolve(ticket.session->path_id));
    co_return co_await typed(ticket,
                             EvidenceKind::DocumentLink,
                             "DocumentLink",
                             worker::DocumentLinkParams{std::move(path)},
                             std::move(token));
}

kota::task<std::optional<std::vector<feature::FoldingRange>>, ipc::Error>
    Dispatcher::folding_ranges(const Ticket& ticket, kota::cancellation_token token) {
    auto path = std::string(project.file_table.resolve(ticket.session->path_id));
    co_return co_await typed(ticket,
                             EvidenceKind::FoldingRange,
                             "FoldingRange",
                             worker::FoldingRangeParams{std::move(path)},
                             std::move(token));
}

kota::task<std::optional<std::vector<feature::DocumentSymbol>>, ipc::Error>
    Dispatcher::document_symbols(const Ticket& ticket, kota::cancellation_token token) {
    auto path = std::string(project.file_table.resolve(ticket.session->path_id));
    co_return co_await typed(ticket,
                             EvidenceKind::DocumentSymbol,
                             "DocumentSymbol",
                             worker::DocumentSymbolParams{std::move(path), project.config},
                             std::move(token));
}

kota::task<std::optional<std::vector<feature::SemanticToken>>, ipc::Error>
    Dispatcher::semantic_tokens(const Ticket& ticket, kota::cancellation_token token) {
    auto path = std::string(project.file_table.resolve(ticket.session->path_id));
    co_return co_await typed(ticket,
                             EvidenceKind::SemanticTokens,
                             "SemanticTokens",
                             worker::SemanticTokensParams{std::move(path), project.config},
                             std::move(token));
}

kota::task<std::vector<std::vector<LocalSourceRange>>, ipc::Error>
    Dispatcher::selection_ranges(const Ticket& ticket,
                                 std::vector<std::uint32_t> offsets,
                                 kota::cancellation_token token) {
    auto path = std::string(project.file_table.resolve(ticket.session->path_id));
    co_return co_await typed(ticket,
                             EvidenceKind::SelectionRange,
                             "SelectionRange",
                             worker::SelectionRangeParams{std::move(path), std::move(offsets)},
                             std::move(token));
}

kota::task<std::vector<feature::CodeAction>, ipc::Error>
    Dispatcher::code_actions(const Ticket& ticket,
                             const protocol::Range& range,
                             kota::cancellation_token token) {
    // Clamped against the buffer the ticket was taken on: a buffer that
    // moves before the reply arrives turns the reply into ContentModified.
    auto selection = ticket.session->positions().offset_range(range, PositionEncoding::UTF16);
    auto path = std::string(project.file_table.resolve(ticket.session->path_id));
    co_return co_await typed(ticket,
                             EvidenceKind::CodeAction,
                             "CodeAction",
                             worker::CodeActionParams{std::move(path), selection},
                             std::move(token));
}

template <typename Params>
kota::task<typename protocol::RequestTraits<Params>::Result, ipc::Error>
    Dispatcher::interactive(std::uint8_t evidence,
                            llvm::StringRef label,
                            const Ticket& ticket,
                            protocol::Position position,
                            Params wp,
                            kota::cancellation_token token) {
    using Result = typename protocol::RequestTraits<Params>::Result;
    auto& session = *ticket.session;
    auto path_id = session.path_id;
    auto path = std::string(project.file_table.resolve(path_id));
    // This build parses the same content as the compile: a compile or
    // preamble that crashed bars it as well as its own crashes do.
    auto barred = [&] {
        return session.quarantine->barred(evidence, Quarantine::Clock::now()) ||
               ASTFamily::compile_barred(session);
    };
    if(barred()) {
        LOG_DEBUG("{}: {} is barred by a crash", label, path);
        co_return empty_value<Result>();
    }

    wp.file = path;
    wp.text = session.text().str();
    wp.offset = session.positions().offset_clamped(position, PositionEncoding::UTF16);
    auto resolution = contexts.resolve_command(path_id, wp.directory, wp.arguments);
    wp.config = project.config;

    ScopedTimer timer;
    ASTFamily::StatelessInputs inputs;
    if(!co_await ast.prepare_stateless_inputs(ticket,
                                              wp.text,
                                              wp.directory,
                                              wp.arguments,
                                              resolution,
                                              inputs)) {
        co_await kota::fail(
            ipc::Error{protocol::ErrorCode::InvalidRequest, "the server is shutting down"});
    }
    wp.pch = std::move(inputs.pch);
    wp.pcms = std::move(inputs.pcms);
    // A preamble crash inside the preparation bars the content: stop
    // before dispatching it again.
    if(barred()) {
        LOG_DEBUG("{}: {} was barred during dependency prep", label, path);
        co_return empty_value<Result>();
    }
    auto wait_ms = timer.ms_f();

    // A completion reply stays useful after edits at or past the cursor:
    // its ranges still hold, and the client filters it by what was typed
    // meanwhile.
    auto carried = wp.text.substr(0, wp.offset);
    auto snapshot = [&] {
        return std::is_same_v<Params, worker::CompletionParams> &&
               session.text().starts_with(carried);
    };
    if(!ticket.fresh() && !snapshot()) {
        co_await kota::fail(content_modified());
    }

    if(resolution.synthesized) {
        wp.synthesized = resolution.synthesized->files;
    }
    wp.hidden = std::move(resolution.hidden);

    Quarantine::Attempt attempt(*session.quarantine, evidence);
    auto result = co_await deliver(
        pool,
        false,
        [&] { return pool.send_stateless(wp, worker::Priority::High, token); },
        [&](const ipc::Error& error) { ast.record_crash(ticket.session, evidence, error); });
    result = finish(ticket, evidence, label, std::move(result), snapshot());
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

kota::task<worker::CompletionResult, ipc::Error>
    Dispatcher::completion(const Ticket& ticket,
                           const protocol::Position& position,
                           const feature::CompletionClient& client,
                           std::vector<feature::ScopeCandidate> candidates,
                           bool more_candidates,
                           kota::cancellation_token token) {
    return interactive(evidence_kind(EvidenceKind::Completion),
                       "Completion",
                       ticket,
                       position,
                       worker::CompletionParams{.client = client,
                                                .candidates = std::move(candidates),
                                                .more_candidates = more_candidates},
                       std::move(token));
}

Dispatcher::RawResult Dispatcher::signature_help(const Ticket& ticket,
                                                 const protocol::Position& position,
                                                 std::string selected,
                                                 kota::cancellation_token token) {
    return interactive(evidence_kind(EvidenceKind::SignatureHelp),
                       "SignatureHelp",
                       ticket,
                       position,
                       worker::SignatureHelpParams{.selected = std::move(selected)},
                       std::move(token));
}

Dispatcher::RawResult Dispatcher::format(const Ticket& ticket,
                                         std::optional<protocol::Range> range,
                                         kota::cancellation_token token) {
    auto& session = *ticket.session;
    auto path = std::string(project.file_table.resolve(session.path_id));
    auto evidence = evidence_kind(EvidenceKind::Format);

    if(session.quarantine->barred(evidence, Quarantine::Clock::now())) {
        LOG_DEBUG("Format: {} is barred by a crash", path);
        co_return serde_raw{"null"};
    }

    worker::FormatParams wp;
    wp.file = project.file_table.display(session.path_id);
    wp.text = session.text().str();

    if(range) {
        wp.range = session.positions().offset_range(*range, PositionEncoding::UTF16);
    }

    ScopedTimer timer;
    Quarantine::Attempt attempt(*session.quarantine, evidence);
    auto result = co_await deliver(
        pool,
        false,
        [&] { return pool.send_stateless(wp, worker::Priority::High, token); },
        [&](const ipc::Error& error) { ast.record_crash(ticket.session, evidence, error); });
    result = finish(ticket, evidence, "Format", std::move(result));
    if(!result.has_value()) {
        co_return kota::outcome_error(std::move(result.error()));
    }
    LOG_PERF("request", "kind=Format file={} total_ms={:.2f}", path, timer.ms_f());
    if(!result->error.empty()) {
        co_return kota::outcome_error(
            ipc::Error{static_cast<protocol::integer>(protocol::LSPErrorCodes::RequestFailed),
                       std::move(result->error)});
    }
    auto positions = session.positions();
    std::vector<protocol::TextEdit> edits;
    for(auto& edit: result->edits) {
        if(auto range = positions.range(edit.range, PositionEncoding::UTF16)) {
            edits.push_back({.range = *range, .new_text = std::move(edit.text)});
        }
    }
    co_return to_raw(edits);
}

}  // namespace clice

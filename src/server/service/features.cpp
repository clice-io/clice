#include "server/service/features.h"

#include <algorithm>
#include <format>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "command/search_config.h"
#include "sched/context.h"
#include "sched/index/pump.h"
#include "semantic/symbol.h"
#include "server/protocol/lsp_projection.h"
#include "server/service/ast_family.h"
#include "syntax/completion.h"
#include "syntax/include_resolver.h"
#include "worker/protocol.h"
#include "worker/serialize.h"

#include "kota/codec/json/json.h"
#include "llvm/ADT/STLExtras.h"

namespace clice {

using serde_raw = kota::codec::RawValue;

/// Error response for feature requests on files with no open session.
static kota::ipc::Error document_not_open() {
    return kota::ipc::Error{kota::ipc::protocol::ErrorCode::InvalidParams, "Document not open"};
}

/// Error response when a call/type hierarchy item cannot be resolved back to
/// an indexed symbol.
static kota::ipc::Error item_not_resolved(llvm::StringRef kind) {
    return kota::ipc::Error{kota::ipc::protocol::ErrorCode::InvalidParams,
                            std::format("Failed to resolve {} item", kind)};
}

bool Features::ast_answerable(const Session& session) const {
    return ast.projections.index_current(session.path_id) && !session.quarantine.blocked();
}

kota::task<Features::Route> Features::pick_route(const Ticket& ticket,
                                                 bool full_lex,
                                                 ServingSource* source) {
    // This handler is resumed eagerly: drain the transport pipe before
    // reading any buffer state, so a queued didChange or cancel lands
    // first (the same discipline as completion's yield).
    co_await kota::yield();
    if(!ticket.fresh()) {
        co_return Route::Superseded;
    }
    auto& session = *ticket.session;
    if(ast_answerable(session)) {
        co_return Route::Ast;
    }
    // An oversized buffer is not worth a synchronous main-thread lex; the
    // full-lex projections follow the investment policy instead of the
    // index slice. Row-backed answers serve at any size.
    constexpr std::size_t full_lex_cap = 8 * 1024 * 1024;
    bool capped = full_lex && session.text.size() > full_lex_cap;
    if(!capped) {
        // The session's own rows when current (the quarantine fallback —
        // the worker cannot be asked, the rows are still true), else the
        // disk shard admitted by freshness clause 4.
        if(auto serving = query.serving_source(session.path_id)) {
            // An index answer must not strand an escalated session's pending
            // build: this request still pulls the compile it would otherwise
            // have waited on, just without blocking.
            if(session.serving == ServingMode::Escalated &&
               !ast.projections.current(session.path_id)) {
                ast.request_compile(ticket.session);
            }
            if(source) {
                *source = serving;
            }
            co_return Route::Index;
        }
    }
    co_return session.serving == ServingMode::Escalated ? Route::Ast : Route::Empty;
}

Features::RawResult Features::stop_reply(Stop stop) {
    if(stop.error) {
        co_return kota::outcome_error(std::move(*stop.error));
    }
    co_return serde_raw{"null"};
}

kota::task<std::optional<Features::Stop>> Features::nav_gate(const Ticket& ticket) {
    switch(co_await pick_route(ticket, /*full_lex=*/false)) {
        case Route::Superseded: co_return Stop{content_modified()};
        // The index sources resolve the cursor under clauses 1/4 — or
        // reject it, which the query layer answers as empty. Either way
        // no compile is owed.
        case Route::Index:
        case Route::Empty: co_return std::nullopt;
        case Route::Ast: break;
    }
    // Same posture as every AST-backed request: the session's file index
    // is produced by the very compile awaited here, so once this settles
    // the index describes the buffer. A failed compile yields null rather
    // than a lookup against a buffer with no settled index; a superseded
    // one tells the client to re-pull.
    bool compiled = co_await ast.ensure_compiled(ticket.session);
    if(!ticket.fresh()) {
        co_return Stop{content_modified()};
    }
    if(!compiled) {
        co_return Stop{};
    }
    co_return std::nullopt;
}

std::optional<IndexQuery::Cursor> Features::cursor_at(Fid path_id,
                                                      const protocol::Position& position) const {
    auto serving = query.serving_source(path_id);
    if(!serving) {
        return std::nullopt;
    }
    auto offset = serving.coords.to_offset(position);
    if(!offset) {
        return std::nullopt;
    }
    return query.symbol_at(path_id, *offset);
}

/// The language selectors of a file's CDB entry: what the last -x forces,
/// if any (the driver override beats every suffix heuristic), and the
/// last -std value. Rules applied, like the resolve path's effective
/// command.
struct CommandLang {
    std::optional<bool> forces_c;
    std::string standard;
};

static std::optional<CommandLang> command_lang(Workspace& workspace, llvm::StringRef path) {
    auto candidates = workspace.cdb.candidate_entries(path);
    if(candidates.empty()) {
        return std::nullopt;
    }
    std::vector<std::string> append, remove;
    workspace.config.match_rules(path, append, remove);
    auto applied =
        workspace.cdb.apply_rules(candidates.front().config, {.remove = remove, .append = append});

    CommandLang result;
    auto language = workspace.cdb.forced_language(applied);
    if(!language.empty()) {
        result.forces_c = language == "c" || language == "c-header";
    }
    for(auto& arg: workspace.cdb.config(applied).args) {
        if(arg.opt_id == option::OPT_std_EQ && arg.values.size() == 1) {
            result.standard = arg.values[0];
        }
    }
    return result;
}

const clang::LangOptions& Features::index_lang_options(const Session& session) {
    auto path = workspace.file_table.resolve(session.path_id);
    auto own = command_lang(workspace, path);
    if(own && own->forces_c) {
        return feature::index_lang_options("", *own->forces_c, own->standard);
    }

    // A header's active context (the user's persisted choice, else the
    // resolved host) names the view being read; its command beats the
    // contributor union the way it does for the AST after an escalation.
    Fid host;
    if(auto it = contexts.saved_contexts.find(session.path_id);
       it != contexts.saved_contexts.end()) {
        host = it->second.host_path_id;
    }
    if(!host.valid()) {
        if(const auto* context = contexts.header_context(session.path_id)) {
            host = context->host_path_id;
        }
    }
    if(host.valid()) {
        auto host_path = workspace.file_table.resolve(host);
        auto host_lang = command_lang(workspace, host_path);
        if(host_lang && host_lang->forces_c) {
            return feature::index_lang_options("", *host_lang->forces_c, host_lang->standard);
        }
        return feature::index_lang_options(path,
                                           host_path.ends_with(".c"),
                                           host_lang ? llvm::StringRef(host_lang->standard)
                                                     : llvm::StringRef());
    }

    auto& contributions = workspace.project_index.contributions;
    auto it = contributions.find(session.path_id);
    bool c_rows = it != contributions.end() && !it->second.empty() &&
                  llvm::all_of(llvm::make_first_range(it->second), [&](Fid tu) {
                      return workspace.file_table.resolve(tu).ends_with(".c");
                  });
    return feature::index_lang_options(path,
                                       c_rows,
                                       own ? llvm::StringRef(own->standard) : llvm::StringRef());
}

std::optional<feature::HoverInfo> Features::index_hover_card(const Session& session,
                                                             const protocol::Position& position) {
    auto cursor = cursor_at(session.path_id, position);
    if(!cursor) {
        return std::nullopt;
    }
    auto info = query.symbol_info(cursor->symbol);
    if(!info) {
        return std::nullopt;
    }
    std::string definition;
    std::string comment;
    if(auto text = query.definition_text(cursor->symbol)) {
        definition = std::move(text->text);
        comment = std::move(text->comment);
    }
    auto hover = feature::index_hover(*info, definition, comment);
    hover.symbol_range = cursor->site.range;
    return hover;
}

std::vector<feature::DocumentLink> Features::find_preamble_links(const Session& session) {
    auto state = query.preamble_blob(session);
    return state ? state->links() : std::vector<feature::DocumentLink>{};
}

std::vector<protocol::Location>
    Features::resolve_directive_definition(Session& session, const protocol::Position& position) {
    std::vector<protocol::Location> locations;

    // Preamble include lines: compiled into the PCH, invisible to the
    // worker's AST — the PCH's stored links carry the targets.
    auto links = find_preamble_links(session);
    if(links.empty())
        return locations;

    auto offset = session.line_map().to_offset(position);
    if(!offset)
        return locations;

    for(auto& link: links) {
        /// Link ranges are half-open; contains() would also accept end.
        if(*offset >= link.range.begin && *offset < link.range.end) {
            locations.push_back(protocol::Location{
                .uri = feature::to_uri(link.target),
                .range = protocol::Range{},
            });
            break;
        }
    }
    return locations;
}

std::optional<protocol::Hover>
    Features::resolve_preamble_hover(Session& session, const protocol::Position& position) {
    auto links = find_preamble_links(session);
    if(links.empty())
        return std::nullopt;

    auto map = session.line_map();
    auto offset = map.to_offset(position);
    if(!offset)
        return std::nullopt;

    for(const auto& link: links) {
        if(*offset < link.range.begin || *offset >= link.range.end)
            continue;

        if(link.range.end > session.text.size())
            return std::nullopt;

        llvm::StringRef name(session.text.data() + link.range.begin, link.range.length());
        name = name.trim();
        if(name.size() >= 2 && ((name.front() == '"' && name.back() == '"') ||
                                (name.front() == '<' && name.back() == '>'))) {
            name = name.drop_front().drop_back();
        }

        feature::HoverInfo info;
        info.name = name.str();
        info.kind = SymbolKind::Header;
        info.definition = link.target;
        info.symbol_range = link.range;

        auto hover = feature::to_protocol_hover(info, workspace.config.hover, map);
        if(!hover.range)
            return std::nullopt;
        return hover;
    }
    return std::nullopt;
}

kota::task<std::vector<protocol::DocumentLink>, kota::ipc::Error>
    Features::document_links(std::shared_ptr<Session> session,
                             std::optional<kota::cancellation_token> token) {
    auto ticket = Ticket::take(session);

    // Links carry byte offsets; this reply edge converts them.
    auto convert = [&](llvm::ArrayRef<feature::DocumentLink> raw_links,
                       std::vector<protocol::DocumentLink>& links) {
        auto map = session->line_map();
        for(const auto& link: raw_links) {
            auto range = map.to_range(link.range.begin, link.range.end);
            if(!range)
                continue;
            protocol::DocumentLink out{.range = *range};
            out.target = feature::to_uri(link.target);
            out.tooltip = link.target;
            links.push_back(std::move(out));
        }
    };

    for(bool waited = false;;) {
        switch(co_await pick_route(ticket, /*full_lex=*/false)) {
            case Route::Superseded: co_return kota::outcome_error(content_modified());
            case Route::Index: {
                // Manifest edges cover the whole document (the background
                // index has no preamble split); guard-skipped lines and
                // __has_include/#embed have no edge and produce no link.
                // Unlike the rows the other projections serve, the edges are
                // disk truth: the quarantine fallback (own index current,
                // buffer edited) must not project them onto a buffer the
                // manifest never described — an edited directive would get
                // the old target, confidently wrong.
                std::vector<protocol::DocumentLink> links;
                if(query.matching_shard(*session)) {
                    auto raw = feature::index_document_links(session->text,
                                                             index_lang_options(*session),
                                                             query.include_edges(*session));
                    convert(raw, links);
                }
                session->index_served = true;
                co_return links;
            }
            case Route::Empty: {
                // Like the outline, links have no refresh request; a cold
                // document's empty reply would outlive the boost that is
                // about to make them real. Await it once, then answer
                // from the settled state.
                if(!waited) {
                    waited = true;
                    co_await pump.await_attempt(session->path_id);
                    if(ticket.fresh()) {
                        continue;
                    }
                    co_return kota::outcome_error(content_modified());
                }
                co_return std::vector<protocol::DocumentLink>{};
            }
            case Route::Ast: break;
        }
        break;
    }

    auto result = co_await dispatcher.document_links(ticket, std::move(token));
    if(!result.has_value())
        co_return kota::outcome_error(std::move(result.error()));

    // The preamble is compiled into the PCH, so the worker's AST only
    // covers the rest of the file — merge the preamble's links in front.
    std::vector<protocol::DocumentLink> links;
    convert(find_preamble_links(*session), links);
    convert(result.value(), links);
    co_return links;
}

Features::RawResult Features::definition(std::shared_ptr<Session> session,
                                         Fid path_id,
                                         const protocol::Position& position,
                                         std::optional<kota::cancellation_token> token) {
    Ticket ticket;
    if(session) {
        ticket = Ticket::take(session);
        if(auto stop = co_await nav_gate(ticket)) {
            co_return co_await stop_reply(std::move(*stop));
        }
    }

    // Preamble include lines first: they have no symbol occurrence in
    // the index and are invisible to the worker's AST. A projection still
    // not current after the awaited compile means the world was re-dirtied
    // mid-flight (the round landed as bounded staleness): the cached
    // links may describe a pre-edit preamble — skip, and let the index and
    // worker paths below answer.
    if(session && ast.projections.current(path_id)) {
        if(auto directive = resolve_directive_definition(*session, position); !directive.empty()) {
            co_return to_raw(directive);
        }
    }

    // The eager index query is safe for dirty sessions too: freshness
    // clause 4 serves their shard only while the buffer is byte-identical,
    // and rejects the mixed-view lookup otherwise.
    auto index_definition = [&]() -> std::vector<protocol::Location> {
        auto cursor = cursor_at(path_id, position);
        return cursor ? to_lsp::locations(query.definition(*cursor))
                      : std::vector<protocol::Location>{};
    };
    if(auto result = index_definition(); !result.empty()) {
        co_return to_raw(result);
    }

    if(!session)
        co_return kota::outcome_error(document_not_open());

    // An index-only session never owes the compile the worker dispatch
    // implies, a session served under freshness clause 4 (escalated,
    // compile still in flight) already routed to the index, and a
    // quarantined session cannot reach a worker at all. What the worker
    // leg covers — include directives, which have no symbol occurrence —
    // the manifest edges answer instead, under the same content gate as
    // the links projection: manifest lines are meaningless against a
    // buffer the index never described.
    if(session->serving == ServingMode::IndexOnly || session->quarantine.blocked() ||
       query.serving_source(path_id).by == ServingSource::By::ShardAsClosed) {
        if(!query.matching_shard(*session)) {
            co_return serde_raw{"[]"};
        }
        if(auto offset = session->line_map().to_offset(position)) {
            auto links = feature::index_document_links(session->text,
                                                       index_lang_options(*session),
                                                       query.include_edges(*session));
            for(const auto& link: links) {
                if(*offset >= link.range.begin && *offset < link.range.end) {
                    std::vector<protocol::Location> locations{
                        protocol::Location{
                                           .uri = feature::to_uri(link.target),
                                           .range = protocol::Range{},
                                           }
                    };
                    co_return to_raw(locations);
                }
            }
        }
        co_return serde_raw{"[]"};
    }

    auto raw = co_await dispatcher.query(worker::QueryKind::GoToDefinition,
                                         ticket,
                                         position,
                                         {},
                                         std::move(token));
    // A dispatch error is final: a ContentModified in particular must not
    // be replaced by an index answer computed on the newer buffer.
    if(!raw.has_value()) {
        co_return kota::outcome_error(std::move(raw.error()));
    }
    if(!to_lsp::is_empty(raw.value())) {
        co_return std::move(raw.value());
    }

    // The dispatch compiled a dirty buffer: retry against the refreshed
    // projection and preamble links, but only when the compile actually
    // completed — a failed compile leaves the projection non-current and
    // the caches stale.
    if(ast.projections.current(path_id)) {
        if(auto retry = index_definition(); !retry.empty()) {
            co_return to_raw(retry);
        }
        if(auto directive = resolve_directive_definition(*session, position); !directive.empty()) {
            co_return to_raw(directive);
        }
    }
    co_return std::move(raw);
}

Features::RawResult Features::hover(std::shared_ptr<Session> session,
                                    const protocol::Position& position,
                                    std::optional<kota::cancellation_token> token) {
    if(!session) {
        co_return kota::outcome_error(document_not_open());
    }
    auto ticket = Ticket::take(session);
    auto path_id = session->path_id;

    auto index_card = [&]() -> std::optional<serde_raw> {
        if(auto info = index_hover_card(*session, position)) {
            return to_raw(
                feature::to_protocol_hover(*info, workspace.config.hover, session->line_map()));
        }
        return std::nullopt;
    };

    switch(co_await pick_route(ticket, /*full_lex=*/false)) {
        case Route::Superseded: co_return kota::outcome_error(content_modified());
        case Route::Index: {
            if(auto card = index_card()) {
                session->index_served = true;
                co_return std::move(*card);
            }
            co_return serde_raw{"null"};
        }
        case Route::Empty: co_return serde_raw{"null"};
        case Route::Ast: break;
    }

    /// Like document_links, the preamble and the worker's AST are disjoint, so merge the two
    /// sources.
    auto raw =
        co_await dispatcher.query(worker::QueryKind::Hover, ticket, position, {}, std::move(token));
    if(raw.has_value() && to_lsp::is_null(raw.value()) && ast.projections.current(path_id)) {
        if(auto hover = resolve_preamble_hover(*session, position)) {
            co_return to_raw(*hover);
        }
        // The preamble region is the one place a null from the AST is not
        // authoritative — it is compiled into the PCH and invisible to
        // the worker (a `#define` there has no node). The index card
        // fills that gap, and only that gap: past the bound the AST saw
        // the code and declined on purpose (a lambda's auto parameter
        // must not hover, and the index would happily name it `auto:1`).
        auto projection = ast.projections.projection(path_id);
        if(projection && projection->pch_key) {
            if(auto it = workspace.pch_cache.find(*projection->pch_key);
               it != workspace.pch_cache.end()) {
                auto offset = session->line_map().to_offset(position);
                if(offset && *offset < it->second.bound) {
                    if(auto card = index_card()) {
                        co_return std::move(*card);
                    }
                }
            }
        }
    }
    co_return std::move(raw);
}

Features::RawResult Features::semantic_tokens(std::shared_ptr<Session> session,
                                              std::optional<kota::cancellation_token> token) {
    auto ticket = Ticket::take(session);
    ServingSource source;
    switch(co_await pick_route(ticket, /*full_lex=*/true, &source)) {
        case Route::Superseded: co_return kota::outcome_error(content_modified());
        case Route::Index: {
            auto rows = feature::extract_index_rows(*source.rows);
            auto tokens = feature::index_semantic_tokens(
                session->text,
                index_lang_options(*session),
                rows.occurrences,
                rows.decls,
                [&](index::SymbolHash hash) { return query.symbol_info(hash); });
            session->index_served = true;
            co_return to_raw(
                feature::semantic_tokens_to_protocol(tokens,
                                                     session->text,
                                                     session->line_starts,
                                                     feature::PositionEncoding::UTF16));
        }
        case Route::Empty: {
            // The client caches this null, and only a semanticTokens
            // refresh makes it re-pull once the cold document's shard
            // lands — the merge signals refreshes for sessions with
            // this flag, so an empty pull registers the same interest
            // as a served one.
            session->index_served = true;
            co_return serde_raw{"null"};
        }
        case Route::Ast: break;
    }
    co_return co_await dispatcher.query(worker::QueryKind::SemanticTokens,
                                        ticket,
                                        {},
                                        {},
                                        std::move(token));
}

Features::RawResult Features::inlay_hints(std::shared_ptr<Session> session,
                                          const protocol::Range& range,
                                          std::optional<kota::cancellation_token> token) {
    // Inlay hints are Sema products the index cannot project; a session
    // the policy keeps un-compiled answers honestly empty. The compile
    // that follows an escalation pushes an inlayHint refresh, so clients
    // re-pull once real hints exist.
    if(session->serving == ServingMode::IndexOnly) {
        session->index_served = true;
        co_return serde_raw{"[]"};
    }
    co_return co_await dispatcher.query(worker::QueryKind::InlayHints,
                                        Ticket::take(session),
                                        {},
                                        range,
                                        std::move(token));
}

Features::RawResult Features::folding_range(std::shared_ptr<Session> session,
                                            std::optional<kota::cancellation_token> token) {
    auto ticket = Ticket::take(session);
    ServingSource source;
    switch(co_await pick_route(ticket, /*full_lex=*/true, &source)) {
        case Route::Superseded: co_return kota::outcome_error(content_modified());
        case Route::Index: {
            auto rows = feature::extract_index_rows(*source.rows);
            auto folds = feature::index_folding_ranges(
                session->text,
                index_lang_options(*session),
                rows.decls,
                [&](index::SymbolHash hash) { return query.symbol_info(hash); });
            session->index_served = true;
            co_return to_raw(feature::folding_ranges_to_protocol(folds,
                                                                 session->text,
                                                                 session->line_starts,
                                                                 feature::PositionEncoding::UTF16));
        }
        case Route::Empty: {
            // Same contract as the semantic-tokens Empty route: the
            // client caches this reply, and only a foldingRange
            // refresh makes it re-pull once the cold document's
            // shard lands.
            session->index_served = true;
            co_return serde_raw{"[]"};
        }
        case Route::Ast: break;
    }
    co_return co_await dispatcher.query(worker::QueryKind::FoldingRange,
                                        ticket,
                                        {},
                                        {},
                                        std::move(token));
}

Features::RawResult Features::document_symbol(std::shared_ptr<Session> session,
                                              std::optional<kota::cancellation_token> token) {
    auto ticket = Ticket::take(session);
    for(bool waited = false;;) {
        ServingSource source;
        switch(co_await pick_route(ticket, /*full_lex=*/false, &source)) {
            case Route::Superseded: co_return kota::outcome_error(content_modified());
            case Route::Index: {
                auto rows = feature::extract_index_rows(*source.rows);
                auto symbols =
                    feature::index_document_symbols(rows.decls, [&](index::SymbolHash hash) {
                        return query.symbol_info(hash);
                    });
                session->index_served = true;
                co_return to_raw(
                    feature::document_symbols_to_protocol(symbols,
                                                          session->text,
                                                          session->line_starts,
                                                          feature::PositionEncoding::UTF16));
            }
            case Route::Empty: {
                // The outline has no workspace refresh request: an
                // empty reply to a cold document would be cached by
                // the client for good — no signal ever makes it
                // re-pull. Await the didOpen boost instead (bounded
                // by one index attempt) and answer from whatever
                // source that settles: the shard, the escalated
                // compile, or honestly empty under readonly "on".
                if(!waited) {
                    waited = true;
                    co_await pump.await_attempt(session->path_id);
                    if(ticket.fresh()) {
                        continue;
                    }
                    co_return kota::outcome_error(content_modified());
                }
                co_return serde_raw{"[]"};
            }
            case Route::Ast: break;
        }
        break;
    }
    co_return co_await dispatcher.query(worker::QueryKind::DocumentSymbol,
                                        ticket,
                                        {},
                                        {},
                                        std::move(token));
}

Features::RawResult Features::code_action(std::shared_ptr<Session> session,
                                          std::optional<kota::cancellation_token> token) {
    // Code actions are AST products with no index projection; a session
    // the policy keeps un-compiled answers honestly empty rather than
    // forcing the compile the policy declined.
    if(!ast_answerable(*session) && session->serving == ServingMode::IndexOnly) {
        co_return serde_raw{"[]"};
    }
    co_return co_await dispatcher.query(worker::QueryKind::CodeAction,
                                        Ticket::take(session),
                                        {},
                                        {},
                                        std::move(token));
}

Features::RawResult Features::completion(std::shared_ptr<Session> session,
                                         const protocol::Position& position,
                                         llvm::StringRef trigger_character,
                                         std::optional<kota::cancellation_token> token) {
    auto pause = pump.scoped_pause();

    // Asking for code completion is edit intent: flip the session out of
    // index-only serving (a no-op when already escalated or under
    // readonly = "on"). Before the yield below on purpose: it must tag
    // the session this request arrived for, not whatever a drained
    // didClose/didOpen pair put in its place.
    ast.escalate(*session);

    // This handler is resumed eagerly, so a $/cancelRequest or didChange
    // sitting in the pipe (rapid-fire completions cancel and re-issue as
    // the user types) has not been read yet. Yield once BEFORE reading any
    // buffer state: the loop drains the pipe — a fired token tears this
    // frame down here, and an edit lands before the offset and completion
    // context are computed, so the synchronous include scan below never
    // serves candidates or ranges for a buffer that no longer exists.
    // Unlike the whole-document features, a drained edit is served, not
    // refused: the ticket is taken after the drain, and the client
    // filters candidates against whatever it typed meanwhile.
    co_await kota::yield();

    // The drain may also have replaced the Session object (a didClose,
    // with or without a reopen): the request belongs to the discarded
    // buffer, and only the store can tell.
    if(sessions.find(session->path_id) != session) {
        co_return kota::outcome_error(content_modified());
    }
    auto ticket = Ticket::take(session);

    auto path_id = session->path_id;
    auto path = std::string(workspace.file_table.resolve(path_id));

    auto map = session->line_map();
    auto offset = map.to_offset(position);

    PreambleCompletionContext pctx;
    if(offset) {
        pctx = detect_completion_context(session->text, *offset);
    }

    // Space is advertised as a trigger character only so that `import `
    // opens module suggestions. Clients without request-side gating
    // (nvim, zed) forward every space keystroke; answer everything else
    // with an empty list before any include scanning or completion build.
    if(trigger_character == " " && pctx.kind != CompletionContext::Import) {
        co_return serde_raw{"[]"};
    }

    if(offset) {
        if(pctx.kind == CompletionContext::IncludeQuoted ||
           pctx.kind == CompletionContext::IncludeAngled) {
            std::string directory;
            std::vector<std::string> arguments;
            // Editor use: candidates must come from the same command (host
            // choice, chosen CDB entry) the open buffer compiles under.
            CommandRef ref;
            contexts.resolve_command(path,
                                     directory,
                                     arguments,
                                     ContextUse::Editor,
                                     /*host_path_id=*/nullptr,
                                     /*extra_prepend=*/{},
                                     /*extra_append=*/{},
                                     &ref);

            auto search_config = workspace.cdb.search_config(ref);
            DirListingCache dir_cache;
            dir_cache.shared = &workspace.file_table;
            auto resolved = resolve_search_config(search_config, dir_cache);
            bool angled = (pctx.kind == CompletionContext::IncludeAngled);
            auto candidates = complete_include_path(resolved, pctx.prefix, angled, dir_cache);

            std::vector<protocol::CompletionItem> items;
            items.reserve(candidates.size());
            for(auto& c: candidates) {
                protocol::CompletionItem item;
                item.label = c.is_directory ? c.name + "/" : c.name;
                item.kind = protocol::CompletionItemKind::File;
                items.push_back(std::move(item));
            }
            auto json = kota::codec::json::to_string<kota::ipc::lsp_config>(items);
            co_return serde_raw{json ? std::move(*json) : "[]"};
        }
        if(pctx.kind == CompletionContext::Import) {
            auto module_names = complete_module_import(workspace.dep_graph, pctx.prefix);

            std::vector<protocol::CompletionItem> items;
            items.reserve(module_names.size());
            for(auto& name: module_names) {
                protocol::CompletionItem item;
                item.label = name;
                item.kind = protocol::CompletionItemKind::Module;
                item.insert_text = name + ";";
                items.push_back(std::move(item));
            }
            auto json = kota::codec::json::to_string<kota::ipc::lsp_config>(items);
            co_return serde_raw{json ? std::move(*json) : "[]"};
        }
    }

    co_return co_await dispatcher.completion(ticket, position, std::move(token));
}

Features::RawResult Features::signature_help(std::shared_ptr<Session> session,
                                             const protocol::Position& position,
                                             std::optional<kota::cancellation_token> token) {
    auto pause = pump.scoped_pause();
    ast.escalate(*session);
    co_return co_await dispatcher.signature_help(Ticket::take(session), position, std::move(token));
}

Features::RawResult Features::formatting(std::shared_ptr<Session> session,
                                         std::optional<kota::cancellation_token> token) {
    auto pause = pump.scoped_pause();
    co_return co_await dispatcher.format(Ticket::take(session), {}, std::move(token));
}

Features::RawResult Features::range_formatting(std::shared_ptr<Session> session,
                                               const protocol::Range& range,
                                               std::optional<kota::cancellation_token> token) {
    auto pause = pump.scoped_pause();
    co_return co_await dispatcher.format(Ticket::take(session), range, std::move(token));
}

Features::RawResult Features::references(std::shared_ptr<Session> session,
                                         Fid path_id,
                                         const protocol::Position& position,
                                         bool include_declaration) {
    if(session) {
        if(auto stop = co_await nav_gate(Ticket::take(session))) {
            co_return co_await stop_reply(std::move(*stop));
        }
    }
    auto cursor = cursor_at(path_id, position);
    if(!cursor) {
        co_return serde_raw{"[]"};
    }
    co_return to_raw(to_lsp::locations(query.references(*cursor, include_declaration)));
}

Features::RawResult Features::declaration(std::shared_ptr<Session> session,
                                          Fid path_id,
                                          const protocol::Position& position) {
    if(session) {
        if(auto stop = co_await nav_gate(Ticket::take(session))) {
            co_return co_await stop_reply(std::move(*stop));
        }
    }
    auto cursor = cursor_at(path_id, position);
    if(!cursor) {
        co_return serde_raw{"[]"};
    }
    co_return to_raw(to_lsp::locations(query.declaration(*cursor)));
}

Features::RawResult Features::type_definition(std::shared_ptr<Session> session,
                                              Fid path_id,
                                              const protocol::Position& position) {
    if(session) {
        if(auto stop = co_await nav_gate(Ticket::take(session))) {
            co_return co_await stop_reply(std::move(*stop));
        }
    }
    auto cursor = cursor_at(path_id, position);
    if(!cursor) {
        co_return serde_raw{"[]"};
    }
    co_return to_raw(
        to_lsp::locations(query.target_sites(cursor->symbol, RelationKind::TypeDefinition)));
}

Features::RawResult Features::implementation(std::shared_ptr<Session> session,
                                             Fid path_id,
                                             const protocol::Position& position) {
    if(session) {
        if(auto stop = co_await nav_gate(Ticket::take(session))) {
            co_return co_await stop_reply(std::move(*stop));
        }
    }
    auto cursor = cursor_at(path_id, position);
    if(!cursor) {
        co_return serde_raw{"[]"};
    }
    co_return to_raw(to_lsp::locations(query.implementation(cursor->symbol)));
}

/// The prepared item stands for the symbol, not the cursor: anchor it at
/// the symbol's canonical site so expanding from a use renders the same
/// root as expanding from the declaration.
template <typename Item>
static std::optional<Item> prepared_item(const IndexQuery& query,
                                         const SymbolRef& symbol,
                                         const Site& cursor,
                                         std::optional<Item> (*project)(const SymbolRef&,
                                                                        const Site&)) {
    auto site = query.canonical_site(symbol.hash);
    return project(symbol, site ? *site : cursor);
}

Features::RawResult Features::call_hierarchy_prepare(std::shared_ptr<Session> session,
                                                     Fid path_id,
                                                     const protocol::Position& position) {
    if(session) {
        if(auto stop = co_await nav_gate(Ticket::take(session))) {
            co_return co_await stop_reply(std::move(*stop));
        }
    }
    auto cursor = cursor_at(path_id, position);
    if(!cursor)
        co_return serde_raw{"null"};
    auto info = query.symbol_info(cursor->symbol);
    if(!info)
        co_return serde_raw{"null"};
    if(!(info->kind == SymbolKind::Function || info->kind == SymbolKind::Method ||
         info->kind == SymbolKind::Operator))
        co_return serde_raw{"null"};

    auto item = prepared_item(query, *info, cursor->site, &to_lsp::call_hierarchy_item);
    if(!item)
        co_return serde_raw{"null"};
    std::vector<protocol::CallHierarchyItem> items{std::move(*item)};
    co_return to_raw(items);
}

/// The symbol a hierarchy item stands for: its handle when intact, else
/// the symbol at the item's recorded range in the file's serving source.
static std::optional<index::SymbolHash> item_symbol(const IndexQuery& query,
                                                    std::optional<IndexQuery::Cursor> at_range,
                                                    const std::optional<protocol::LSPAny>& data) {
    if(auto hash = to_lsp::hierarchy_symbol(data); hash && query.symbol_info(*hash)) {
        return hash;
    }
    if(at_range) {
        return at_range->symbol;
    }
    return std::nullopt;
}

Features::RawResult Features::call_hierarchy_incoming(Fid path_id,
                                                      const protocol::CallHierarchyItem& item) {
    auto symbol = item_symbol(query, cursor_at(path_id, item.range.start), item.data);
    if(!symbol)
        co_return kota::outcome_error(item_not_resolved("call hierarchy"));

    std::vector<protocol::CallHierarchyIncomingCall> results;
    for(auto& group: query.grouped(*symbol, RelationKind::Caller)) {
        auto caller = query.resolve(group.symbol);
        if(!caller)
            continue;
        auto from = to_lsp::call_hierarchy_item(caller->symbol, caller->site);
        if(!from)
            continue;
        std::vector<protocol::Range> ranges;
        for(auto& site: group.sites) {
            if(auto range = to_lsp::range(site))
                ranges.push_back(*range);
        }
        results.push_back({std::move(*from), std::move(ranges)});
    }
    co_return to_raw(results);
}

Features::RawResult Features::call_hierarchy_outgoing(Fid path_id,
                                                      const protocol::CallHierarchyItem& item) {
    auto symbol = item_symbol(query, cursor_at(path_id, item.range.start), item.data);
    if(!symbol)
        co_return kota::outcome_error(item_not_resolved("call hierarchy"));

    std::vector<protocol::CallHierarchyOutgoingCall> results;
    for(auto& group: query.grouped(*symbol, RelationKind::Callee)) {
        auto callee = query.resolve(group.symbol);
        if(!callee)
            continue;
        auto to = to_lsp::call_hierarchy_item(callee->symbol, callee->site);
        if(!to)
            continue;
        std::vector<protocol::Range> ranges;
        for(auto& site: group.sites) {
            if(auto range = to_lsp::range(site))
                ranges.push_back(*range);
        }
        results.push_back({std::move(*to), std::move(ranges)});
    }
    co_return to_raw(results);
}

Features::RawResult Features::type_hierarchy_prepare(std::shared_ptr<Session> session,
                                                     Fid path_id,
                                                     const protocol::Position& position) {
    if(session) {
        if(auto stop = co_await nav_gate(Ticket::take(session))) {
            co_return co_await stop_reply(std::move(*stop));
        }
    }
    auto cursor = cursor_at(path_id, position);
    if(!cursor)
        co_return serde_raw{"null"};
    auto info = query.symbol_info(cursor->symbol);
    if(!info)
        co_return serde_raw{"null"};
    if(!(info->kind == SymbolKind::Class || info->kind == SymbolKind::Struct ||
         info->kind == SymbolKind::Enum || info->kind == SymbolKind::Union))
        co_return serde_raw{"null"};

    auto item = prepared_item(query, *info, cursor->site, &to_lsp::type_hierarchy_item);
    if(!item)
        co_return serde_raw{"null"};
    std::vector<protocol::TypeHierarchyItem> items{std::move(*item)};
    co_return to_raw(items);
}

/// The items of every resolvable relation target of `symbol`.
static std::vector<protocol::TypeHierarchyItem> type_items(const IndexQuery& query,
                                                           index::SymbolHash symbol,
                                                           RelationKind kind) {
    std::vector<protocol::TypeHierarchyItem> results;
    for(auto target: query.targets(symbol, kind)) {
        auto located = query.resolve(target);
        if(!located)
            continue;
        if(auto item = to_lsp::type_hierarchy_item(located->symbol, located->site))
            results.push_back(std::move(*item));
    }
    return results;
}

Features::RawResult Features::type_hierarchy_supertypes(Fid path_id,
                                                        const protocol::TypeHierarchyItem& item) {
    auto symbol = item_symbol(query, cursor_at(path_id, item.range.start), item.data);
    if(!symbol)
        co_return kota::outcome_error(item_not_resolved("type hierarchy"));
    co_return to_raw(type_items(query, *symbol, RelationKind::Base));
}

Features::RawResult Features::type_hierarchy_subtypes(Fid path_id,
                                                      const protocol::TypeHierarchyItem& item) {
    auto symbol = item_symbol(query, cursor_at(path_id, item.range.start), item.data);
    if(!symbol)
        co_return kota::outcome_error(item_not_resolved("type hierarchy"));
    co_return to_raw(type_items(query, *symbol, RelationKind::Derived));
}

Features::RawResult Features::workspace_symbol(llvm::StringRef text) {
    std::vector<protocol::SymbolInformation> results;
    for(auto& located: query.search(text, 100)) {
        if(auto info = to_lsp::symbol_information(located.symbol, located.site)) {
            results.push_back(std::move(*info));
        }
    }
    co_return to_raw(results);
}

}  // namespace clice

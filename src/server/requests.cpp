module;

#include "support/prelude.h"

module clice;

import :command.search_config;
import :feature.code_actions;
import :feature.code_completion;
import :feature.diagnostics;
import :feature.document_links;
import :feature.document_symbols;
import :feature.folding_ranges;
import :feature.formatting;
import :feature.hierarchy;
import :feature.hover;
import :feature.inlay_hints;
import :feature.navigation;
import :feature.protocol;
import :feature.rename;
import :feature.selection_ranges;
import :feature.signature_help;
import :feature.semantic_tokens;
import :feature.workspace_symbols;
import :index.peer_queries;
import :sched.index.pump;
import :semantic.symbol;
import :server.ast_family;
import :server.editor_context;
import :server.requests;
import :server.query_commands;
import :support.json;
import :syntax.include_resolver;
import :syntax.source_text;
import :vfs.dir_cache;
import :vfs.file_system;
import :vfs.path;
import :worker.protocol;

namespace clice {

using serde_raw = codec::RawValue;

/// Error response when a call/type hierarchy item cannot be resolved back to
/// an indexed symbol.
static ipc::Error item_not_resolved(llvm::StringRef kind) {
    return ipc::Error{protocol::ErrorCode::InvalidParams,
                      std::format("Failed to resolve {} item", kind)};
}

/// A request the server cannot carry out, and why.
static ipc::Error request_failed(std::string message) {
    return ipc::Error{static_cast<protocol::integer>(protocol::LSPErrorCodes::RequestFailed),
                      std::move(message)};
}

/// The largest buffer the master lexes whole on its own thread.
constexpr std::size_t full_lex_cap = 8 * 1024 * 1024;

bool Requests::ast_answerable(const Session& session) const {
    return ast.compiles.index_current(session.path_id) && !ASTFamily::compile_barred(session);
}

kota::task<Requests::Route> Requests::pick_route(const Ticket& ticket,
                                                 RouteOptions options,
                                                 std::optional<index::IndexSource>* source) {
    for(bool waited = false;;) {
        if(!ticket.fresh()) {
            co_return Route::Superseded;
        }
        auto& session = *ticket.session;
        if(ast_answerable(session)) {
            co_return Route::Ast;
        }
        // An oversized buffer is not worth a synchronous main-thread lex;
        // the features computed from the index by lexing the whole buffer
        // follow the investment policy instead. Answers that only look the
        // index up serve at any size.
        bool capped = options.full_lex && session.text().size() > full_lex_cap;
        if(!capped) {
            // The session's own index when current (the quarantine fallback
            // — the worker cannot be asked, the index is still true), else
            // the disk shard admitted by freshness clause 4.
            auto serving = query.serving(session.path_id);
            // The disk shard merges every context the file compiles in, or
            // holds its first command's: a context the user chose waits for
            // its own compile.
            if(serving && serving->kind == index::IndexSource::Kind::Shard &&
               session.serving == ServingMode::Escalated && contexts.selection(session.path_id)) {
                serving.reset();
            }
            if(serving) {
                // An index answer must not strand an escalated session's
                // pending build: this request still pulls the compile it
                // would otherwise have waited on, just without blocking.
                if(session.serving == ServingMode::Escalated &&
                   !ast.compiles.current(session.path_id)) {
                    ast.request_compile(ticket.session);
                }
                if(source) {
                    *source = serving;
                }
                co_return Route::Index;
            }
        }
        if(session.serving == ServingMode::Escalated) {
            co_return Route::Ast;
        }
        if(options.await_cold_attempt && !waited) {
            waited = true;
            co_await pump.await_attempt(session.path_id);
            continue;
        }
        co_return Route::Empty;
    }
}

Requests::RawResult Requests::stop_reply(Stop stop) {
    if(stop.error) {
        co_await kota::fail(std::move(*stop.error));
    }
    co_return serde_raw{"null"};
}

kota::task<std::optional<Requests::Stop>> Requests::nav_gate(const Ticket& ticket) {
    switch(co_await pick_route(ticket, {})) {
        case Route::Superseded: co_return Stop{content_modified()};
        // The index sources resolve the cursor under clauses 1/4 — or
        // reject it, which the query layer answers as empty. Either way
        // no compile is owed.
        case Route::Index:
        case Route::Empty: co_return std::nullopt;
        case Route::Ast: break;
    }
    // Same posture as every AST-backed request: the session's file index
    // is produced by the very compile awaited here, so once this finishes
    // the index describes the buffer.
    co_return co_await await_compile(ticket);
}

kota::task<std::optional<Requests::Stop>> Requests::await_compile(const Ticket& ticket) {
    // A failed compile yields null rather than a lookup against a buffer
    // with no finished index; a superseded one tells the client to re-pull.
    bool compiled = co_await ast.ensure_compiled(ticket.session);
    if(!ticket.fresh()) {
        co_return Stop{content_modified()};
    }
    if(!compiled) {
        co_return Stop{};
    }
    co_return std::nullopt;
}

std::optional<index::IndexQuery::Cursor>
    Requests::cursor_at(Fid path_id, const protocol::Position& position) const {
    // A document that names no file (an `untitled:` buffer) has no place
    // in the index.
    if(!path_id.valid()) {
        return std::nullopt;
    }
    return query.symbol_at(path_id, position, encoding);
}

template <typename Answer>
Requests::RawResult Requests::navigate(const Ticket& ticket,
                                       Fid path_id,
                                       const protocol::Position& position,
                                       llvm::StringRef none,
                                       Answer answer) {
    if(ticket.session) {
        if(auto stop = co_await nav_gate(ticket)) {
            co_return co_await stop_reply(std::move(*stop));
        }
    }
    auto cursor = cursor_at(path_id, position);
    if(!cursor) {
        co_return serde_raw{none.str()};
    }
    auto reply = answer(*cursor);
    if(!reply) {
        co_await kota::fail(std::move(reply.error()));
    }
    co_return std::move(*reply);
}

index::PeerQueries Requests::peer_queries() const {
    return {.own = query, .peers = peers, .open_in = open_in};
}

SourceText& Requests::text_of(Session& session) {
    session.source->set_dialect(contexts.dialect(session.path_id));
    return *session.source;
}

std::optional<feature::HoverInfo> Requests::index_hover_card(const Session& session,
                                                             const protocol::Position& position) {
    auto cursor = cursor_at(session.path_id, position);
    if(!cursor) {
        return std::nullopt;
    }
    return feature::hover_info(peer_queries(), *cursor, project.config.documentation);
}

std::vector<index::DocumentLink> Requests::find_preamble_links(const Session& session) {
    auto state = query.preamble_index(session.path_id);
    return state ? state->links() : std::vector<index::DocumentLink>{};
}

kota::task<std::vector<index::DocumentLink>, ipc::Error>
    Requests::directive_links(const Ticket& ticket, kota::cancellation_token token) {
    auto result = co_await dispatcher.document_links(ticket, std::move(token)).or_fail();
    // The preamble is compiled into the PCH, so the worker's AST only
    // covers the rest of the file — merge the preamble's links in front.
    auto links = find_preamble_links(*ticket.session);
    links.insert(links.end(), result.begin(), result.end());
    co_return links;
}

Requests::RawResult Requests::document_links(Ticket ticket, kota::cancellation_token token) {
    auto& session = ticket.session;

    switch(co_await pick_route(ticket, {.await_cold_attempt = true})) {
        case Route::Superseded: co_await kota::fail(content_modified());
        case Route::Index: {
            // Manifest edges cover the whole document (the background
            // index has no preamble split); guard-skipped lines and
            // __has_include/#embed have no edge and produce no link.
            auto links = feature::document_links(text_of(*session), query, session->path_id);
            session->index_served = true;
            co_return to_raw(
                feature::to_protocol(links, session->positions(), encoding, project.file_table));
        }
        case Route::Empty: co_return serde_raw{"[]"};
        case Route::Ast: break;
    }

    auto links = co_await directive_links(ticket, std::move(token)).or_fail();
    co_return to_raw(
        feature::to_protocol(links, session->positions(), encoding, project.file_table));
}

kota::task<std::vector<feature::FileDiagnostic>>
    Requests::diagnostics(std::shared_ptr<Session> session) {
    while(session->serving == ServingMode::Escalated && !session->closed) {
        auto ticket = Ticket::take(session);
        co_await ast.ensure_compiled(session);
        if(ticket.fresh()) {
            break;
        }
    }
    co_return finished_diagnostics(*session);
}

std::vector<feature::FileDiagnostic> Requests::finished_diagnostics(const Session& session) const {
    if(session.closed) {
        return {};
    }
    // A compile that failed or is barred left an output of an older
    // buffer, whose ranges a versionless report would place on this one.
    if(auto compiled = ast.compiles.latest_at(session.path_id, session.version)) {
        return shown_diagnostics(session, *compiled->output);
    }
    std::vector<feature::FileDiagnostic> diagnostics;
    append_crash_notes(session, diagnostics);
    return diagnostics;
}

std::vector<feature::FileDiagnostic>
    Requests::shown_diagnostics(const Session& session, const CompileOutput& output) const {
    auto diagnostics = feature::shown_diagnostics(output.diagnostics, project.config.diagnostics);
    append_crash_notes(session, diagnostics);
    return diagnostics;
}

kota::task<std::vector<protocol::CodeAction>, ipc::Error>
    Requests::code_actions(Ticket ticket,
                           const protocol::Range& range,
                           std::vector<protocol::CodeActionKind> only,
                           kota::cancellation_token token) {
    auto& session = ticket.session;
    // Code actions are AST products the index cannot compute; a session
    // the policy keeps un-compiled answers honestly empty rather than
    // forcing the compile the policy declined.
    if(!feature::admits_any(only) ||
       (!ast_answerable(*session) && session->serving == ServingMode::IndexOnly)) {
        co_return std::vector<protocol::CodeAction>{};
    }
    auto actions = co_await dispatcher.code_actions(ticket, range, std::move(token)).or_fail();

    auto path_id = session->path_id;
    auto path = project.file_table.display(path_id);
    Fid host = contexts.host_of(path_id);
    auto host_session = host.valid() ? sessions.find(host) : nullptr;
    auto host_path = host.valid() ? project.file_table.display(host) : std::string();
    // A closed host formats as the build spells it.
    auto host_style_path =
        host_session || !host.valid() ? host_path : project.file_table.spelling(host).str();
    std::optional<SourceText> host_disk;
    std::optional<SearchConfig> search;
    vfs::Scope scope(project.file_table.dirs);
    auto resolved = feature::resolve_actions(
        std::move(actions),
        only,
        {
            .query = &query,
            .file = path_id,
            .path = path,
            .content = session->text(),
            .host = host,
            .host_path = host_path,
            .host_style_path = host_style_path,
            .host_text = [&]() -> SourceText* {
                if(host_session) {
                    return host_session->source.get();
                }
                auto read = vfs::read_text(host_path);
                return read ? &host_disk.emplace(std::move(*read)) : nullptr;
            },
            .include_argument =
                [&](CanonicalRef header) {
                    if(!search) {
                        search = search_of(path_id);
                    }
                    return include_argument(header,
                                            *search,
                                            project.file_table.spelling(path_id),
                                            scope);
                },
        });
    co_return feature::to_protocol(
        std::move(resolved),
        session->positions(),
        encoding,
        {.uri = feature::to_uri(path), .version = session->version},
        {.uri = host.valid() ? feature::to_uri(host_path) : std::string(),
         .version = host_session ? std::optional(host_session->version) : std::nullopt});
}

SearchConfig Requests::search_of(Fid path_id) {
    // Editor use: the search follows the command (host choice, chosen CDB
    // entry) the open buffer compiles under.
    std::string directory;
    std::vector<std::string> arguments;
    return project.toolchain.search_config(
        contexts.resolve_command(path_id, directory, arguments).ref);
}

Requests::RawResult Requests::definition(Ticket ticket,
                                         Fid path_id,
                                         const protocol::Position& position,
                                         kota::cancellation_token token) {
    auto& session = ticket.session;
    if(session) {
        if(auto stop = co_await nav_gate(ticket)) {
            co_return co_await stop_reply(std::move(*stop));
        }
    }

    // Preamble include lines first: they have no symbol occurrence in
    // the index and are invisible to the worker's AST. A latest compile
    // still not current after the awaited one means the world was re-dirtied
    // mid-flight (the round committed as bounded staleness): the cached
    // links may describe a pre-edit preamble — skip, and let the index and
    // worker paths below answer.
    auto offset = session ? session->positions().offset(position, encoding) : std::nullopt;
    if(offset && ast.compiles.current(path_id)) {
        auto links = find_preamble_links(*session);
        if(auto* link = feature::link_at(links, *offset)) {
            co_return to_raw(std::vector{feature::directive_location(project.file_table, *link)});
        }
    }

    // The eager index query is safe for dirty sessions too: freshness
    // clause 4 serves their shard only while the buffer is byte-identical,
    // and rejects the mixed-view lookup otherwise.
    auto index_definition = [&]() -> std::vector<protocol::Location> {
        auto cursor = cursor_at(path_id, position);
        if(!cursor) {
            return {};
        }
        return feature::to_locations(feature::definition(peer_queries(), *cursor), encoding);
    };
    if(auto result = index_definition(); !result.empty()) {
        co_return to_raw(result);
    }

    // A closed file has no worker leg: the index's answer is the answer.
    if(!session)
        co_return serde_raw{"[]"};

    // An index-only session never owes the compile the worker dispatch
    // implies, a session served under freshness clause 4 (escalated,
    // compile still in flight) already routed to the index, and a session
    // whose compile a crash bars cannot reach a worker at all. What the
    // worker leg covers — include directives, which have no symbol
    // occurrence — the manifest edges answer instead, under the same
    // content gate as the index's links: manifest lines are meaningless
    // against a buffer the index never described.
    auto serving = query.serving(path_id);
    bool shard_served = serving && serving->kind == index::IndexSource::Kind::Shard;
    if(session->serving == ServingMode::IndexOnly || ASTFamily::compile_barred(*session) ||
       (shard_served && !cursor_at(path_id, position))) {
        if(offset) {
            auto links = feature::document_links(text_of(*session), query, session->path_id);
            if(auto* link = feature::link_at(links, *offset)) {
                co_return to_raw(
                    std::vector{feature::directive_location(project.file_table, *link)});
            }
        }
        co_return serde_raw{"[]"};
    }

    // A symbol the shard names and no index places waits for the compile:
    // a module's declaration before the module's unit is indexed is in the
    // compile's own index alone. Awaited here, as the links dispatch skips
    // the compile while a crash bars link queries.
    if(shard_served) {
        if(auto stop = co_await await_compile(ticket)) {
            co_return co_await stop_reply(std::move(*stop));
        }
    }

    // A dispatch error is final: a ContentModified in particular must not
    // be replaced by an index answer computed on the newer buffer.
    auto links = co_await directive_links(ticket, std::move(token)).or_fail();
    if(auto* link = offset ? feature::link_at(links, *offset) : nullptr) {
        co_return to_raw(std::vector{feature::directive_location(project.file_table, *link)});
    }

    // The buffer compiled since the first lookup: retry against the refreshed
    // compile, but only when the compile actually completed — a failed
    // compile leaves the latest one non-current and the caches stale.
    if(ast.compiles.current(path_id)) {
        if(auto retry = index_definition(); !retry.empty()) {
            co_return to_raw(retry);
        }
    }
    co_return serde_raw{"[]"};
}

Requests::RawResult Requests::hover(Ticket ticket,
                                    const protocol::Position& position,
                                    protocol::MarkupKind markup,
                                    kota::cancellation_token token) {
    auto& session = ticket.session;
    auto path_id = session->path_id;

    auto card = [&](const feature::HoverInfo& info) {
        return to_raw(feature::to_protocol(info, markup, session->positions(), encoding));
    };

    // A directive's card names its target, which the worker knows only by
    // identity: this side answers it, from the links. Off a directive line
    // there is none to look for.
    auto offset = session->positions().offset(position, encoding);
    bool on_directive =
        offset && feature::on_directive_line(session->text(), *offset, contexts.dialect(path_id));
    auto directive_card =
        [&](llvm::ArrayRef<index::DocumentLink> links) -> std::optional<serde_raw> {
        auto* link = feature::link_at(links, *offset);
        if(!link) {
            return std::nullopt;
        }
        auto hover = feature::directive_hover(session->text(),
                                              session->positions(),
                                              project.file_table,
                                              *link,
                                              markup,
                                              encoding);
        return hover ? to_raw(*hover) : serde_raw{"null"};
    };

    switch(co_await pick_route(ticket, {})) {
        case Route::Superseded: co_await kota::fail(content_modified());
        case Route::Index: {
            if(on_directive) {
                if(auto shown =
                       directive_card(feature::document_links(text_of(*session), query, path_id))) {
                    session->index_served = true;
                    co_return std::move(*shown);
                }
            }
            auto info = index_hover_card(*session, position);
            if(!info) {
                co_return serde_raw{"null"};
            }
            // Without the definition, the index card tells little past the
            // name (the entity the buffer's index recorded can be gone, as
            // after an imported interface changed): while a compile can
            // run, the AST's card is worth the wait.
            if(info->definition.empty() && info->file.empty() &&
               session->serving == ServingMode::Escalated && !ASTFamily::compile_barred(*session)) {
                break;
            }
            session->index_served = true;
            co_return card(*info);
        }
        case Route::Empty: co_return serde_raw{"null"};
        case Route::Ast: break;
    }

    // A module name's card names the unit defining the module, which the
    // worker cannot see across files: this side answers it, from the index.
    if(auto cursor = cursor_at(path_id, position)) {
        auto info = query.symbol_info(cursor->symbols.front(), cursor->site.file);
        if(info && info->kind == SymbolKind::Module) {
            co_return card(feature::module_hover(peer_queries(), *cursor, *info));
        }
    }

    if(on_directive) {
        auto links = co_await directive_links(ticket, token).or_fail();
        if(auto shown = directive_card(links)) {
            co_return std::move(*shown);
        }
    }

    if(auto info = co_await dispatcher.hover(ticket, position, std::move(token)).or_fail()) {
        co_return card(*info);
    }
    // The preamble region is the one place the AST's having no card is not
    // authoritative — it is compiled into the PCH and invisible to the
    // worker (a `#define` there has no node). The index card fills that
    // gap, and only that gap: past the bound the AST saw the code and
    // declined on purpose (a lambda's auto parameter must not hover, and
    // the index would happily name it `auto:1`).
    if(auto compiled = ast.compiles.current(path_id) ? ast.compiles.latest(path_id) : nullptr;
       compiled && compiled->pch_key) {
        if(auto it = project.pch_cache.find(*compiled->pch_key);
           it != project.pch_cache.end() && offset && *offset < it->second.bound) {
            if(auto info = index_hover_card(*session, position)) {
                co_return card(*info);
            }
        }
    }
    co_return serde_raw{"null"};
}

Requests::TokensResult Requests::semantic_tokens(Ticket ticket, kota::cancellation_token token) {
    auto& session = ticket.session;
    std::optional<index::IndexSource> source;
    std::optional<std::vector<feature::SemanticToken>> tokens;
    switch(co_await pick_route(ticket, {.full_lex = true}, &source)) {
        case Route::Superseded: co_await kota::fail(content_modified());
        case Route::Index: {
            session->index_served = true;
            tokens = feature::semantic_tokens(text_of(*session),
                                              query,
                                              *source,
                                              project.config.semantic_tokens);
            break;
        }
        case Route::Empty: {
            // The client caches this null, and only a semanticTokens
            // refresh makes it re-pull once the cold document's shard
            // arrives — the merge signals refreshes for sessions with
            // this flag, so an empty pull registers the same interest
            // as a served one.
            session->index_served = true;
            co_return std::nullopt;
        }
        case Route::Ast: {
            tokens = co_await dispatcher.semantic_tokens(ticket, std::move(token)).or_fail();
            break;
        }
    }
    if(tokens) {
        if(auto preamble = query.preamble_index(session->path_id)) {
            feature::merge_preamble_macros(*preamble, *tokens);
        }
    }
    co_return tokens;
}

Requests::RawResult Requests::inlay_hints(Ticket ticket,
                                          const protocol::Range& range,
                                          bool label_parts,
                                          kota::cancellation_token token) {
    auto& session = ticket.session;
    // Inlay hints are Sema products the index cannot compute; a session
    // the policy keeps un-compiled answers honestly empty. The compile
    // that follows an escalation pushes an inlayHint refresh, so clients
    // re-pull once real hints exist.
    if(session->serving == ServingMode::IndexOnly) {
        session->index_served = true;
        co_return serde_raw{"[]"};
    }
    auto hints = co_await dispatcher.inlay_hints(ticket, range, std::move(token)).or_fail();
    if(!label_parts) {
        co_return to_raw(feature::to_protocol(hints, session->positions(), encoding));
    }
    co_return to_raw(feature::to_protocol(hints,
                                          session->positions(),
                                          encoding,
                                          peer_queries(),
                                          session->path_id));
}

Requests::RawResult Requests::folding_ranges(Ticket ticket,
                                             feature::FoldingClient client,
                                             kota::cancellation_token token) {
    auto& session = ticket.session;
    auto convert = [&](llvm::ArrayRef<feature::FoldingRange> folds) {
        return to_raw(
            feature::to_protocol(folds, session->text(), session->positions(), encoding, client));
    };

    std::optional<index::IndexSource> source;
    switch(co_await pick_route(ticket, {.full_lex = true}, &source)) {
        case Route::Superseded: co_await kota::fail(content_modified());
        case Route::Index: {
            auto folds = feature::folding_ranges(text_of(*session), query, *source);
            session->index_served = true;
            co_return convert(folds);
        }
        case Route::Empty: {
            // Same contract as the semantic-tokens Empty route: the
            // client caches this reply, and only a foldingRange
            // refresh makes it re-pull once the cold document's
            // shard arrives.
            session->index_served = true;
            co_return serde_raw{"[]"};
        }
        case Route::Ast: break;
    }
    auto folds = co_await dispatcher.folding_ranges(ticket, std::move(token)).or_fail();
    if(!folds) {
        co_return serde_raw{"null"};
    }
    co_return convert(*folds);
}

Requests::RawResult Requests::document_symbols(Ticket ticket,
                                               feature::SymbolClient client,
                                               std::string uri,
                                               kota::cancellation_token token) {
    auto& session = ticket.session;
    auto convert = [&](llvm::ArrayRef<feature::DocumentSymbol> symbols) {
        if(client.hierarchical) {
            return to_raw(feature::to_protocol(symbols, session->positions(), encoding, client));
        }
        return to_raw(
            feature::to_symbol_information(symbols, session->positions(), encoding, client, uri));
    };
    std::optional<index::IndexSource> source;
    switch(co_await pick_route(ticket, {.full_lex = true, .await_cold_attempt = true}, &source)) {
        case Route::Superseded: co_await kota::fail(content_modified());
        case Route::Index: {
            auto symbols = feature::document_symbols(text_of(*session),
                                                     query,
                                                     *source,
                                                     project.config.document_symbols);
            session->index_served = true;
            co_return convert(symbols);
        }
        case Route::Empty: co_return serde_raw{"[]"};
        case Route::Ast: break;
    }
    auto symbols = co_await dispatcher.document_symbols(ticket, std::move(token)).or_fail();
    if(!symbols) {
        co_return serde_raw{"null"};
    }
    co_return convert(*symbols);
}

Requests::RawResult Requests::selection_ranges(Ticket ticket,
                                               std::vector<protocol::Position> positions,
                                               kota::cancellation_token token) {
    auto& session = ticket.session;
    std::vector<std::uint32_t> offsets;
    auto map = session->positions();
    for(const auto& position: positions) {
        offsets.push_back(map.offset_clamped(position, encoding));
    }
    auto convert = [&](llvm::ArrayRef<std::vector<LocalSourceRange>> chains) {
        std::vector<protocol::SelectionRange> result;
        for(const auto& chain: chains) {
            result.push_back(feature::to_protocol(chain, session->positions(), encoding));
        }
        return to_raw(result);
    };
    auto lexical = [&] {
        if(session->text().size() > full_lex_cap) {
            std::vector<std::vector<LocalSourceRange>> points;
            for(auto offset: offsets) {
                points.push_back({
                    {offset, offset}
                });
            }
            return convert(points);
        }
        return convert(feature::lexical_selection_ranges(text_of(*session), offsets));
    };

    switch(co_await pick_route(ticket, {})) {
        case Route::Superseded: co_await kota::fail(content_modified());
        case Route::Index:
        case Route::Empty: co_return lexical();
        case Route::Ast: break;
    }
    auto chains = co_await dispatcher.selection_ranges(ticket, offsets, std::move(token)).or_fail();
    if(chains.empty()) {
        co_return lexical();
    }
    co_return convert(chains);
}

Requests::RawResult Requests::code_completion(std::shared_ptr<Session> session,
                                              const protocol::Position& position,
                                              const feature::CompletionClient& client,
                                              llvm::StringRef trigger_character,
                                              kota::cancellation_token token) {
    // Asking for code completion is edit intent: flip the session out of
    // index-only serving (a no-op when already escalated or under
    // readonly = "on"). At dispatch on purpose: it must tag the session
    // this request arrived for, not whatever a didClose/didOpen pair read
    // with it put in its place.
    ast.escalate(*session);
    return complete(std::move(session),
                    pump.scoped_pause(),
                    position,
                    client,
                    trigger_character,
                    std::move(token));
}

Requests::RawResult Requests::complete(std::shared_ptr<Session> session,
                                       IndexPump::ScopedPause pause,
                                       const protocol::Position& position,
                                       const feature::CompletionClient& client,
                                       llvm::StringRef trigger_character,
                                       kota::cancellation_token token) {
    // The task starts once the messages read with the request are
    // dispatched: a $/cancelRequest among them (rapid-fire completions
    // cancel and re-issue as the user types) keeps it from starting, and an
    // edit arrives before the offset and completion context are computed,
    // so the synchronous include scan below never serves candidates or
    // ranges for a buffer that no longer exists. Unlike the whole-document
    // features, such an edit is served, not refused: the ticket is taken
    // here, and the client filters candidates against whatever it typed
    // meanwhile.
    //
    // Those messages may also have replaced the Session object (a
    // didClose, with or without a reopen): the request belongs to the
    // discarded buffer, and only the store can tell.
    if(sessions.find(session->path_id) != session) {
        co_await kota::fail(content_modified());
    }
    auto ticket = Ticket::take(session);

    auto path_id = session->path_id;
    auto path = std::string(project.file_table.resolve(path_id));
    auto positions = session->positions();
    vfs::Scope scope(project.file_table.dirs);
    auto start = feature::start_completion({
        .file = path_id,
        .path = path,
        .text = session->text(),
        .positions = positions,
        .position = position,
        .encoding = encoding,
        .trigger_character = trigger_character,
        .dialect = contexts.dialect(path_id),
        .options = project.config.code_completion,
        .client = client,
        .query = query,
        .graph = project.dep_graph,
        .files = project.file_table,
        .search = [&] { return search_of(path_id); },
        .scope = scope,
    });
    if(std::holds_alternative<feature::NoCompletion>(start)) {
        co_return serde_raw{"[]"};
    }
    if(auto* list = std::get_if<protocol::CompletionList>(&start)) {
        co_return to_raw(*list);
    }
    auto& compile = std::get<feature::CompileCompletion>(start);
    auto result = co_await dispatcher
                      .completion(ticket,
                                  position,
                                  client,
                                  compile.candidates,
                                  compile.more_candidates,
                                  std::move(token))
                      .or_fail();
    if(!result.include) {
        co_return std::move(result.list);
    }

    protocol::CompletionList list;
    [[maybe_unused]] auto decoded = json::from_string<ipc::lsp_config>(result.list.data, list);
    assert(decoded && "the worker encodes the list");
    co_return to_raw(feature::finish_completion(std::move(list),
                                                *result.include,
                                                compile.candidates,
                                                query,
                                                path_id,
                                                search_of(path_id),
                                                scope,
                                                project.config.code_completion.limit));
}

Requests::RawResult Requests::signature_help(std::shared_ptr<Session> session,
                                             const protocol::Position& position,
                                             std::string selected,
                                             kota::cancellation_token token) {
    if(auto offset = session->positions().offset(position, encoding);
       offset &&
       !feature::can_hold_call(session->text(), *offset, contexts.dialect(session->path_id))) {
        return kota::co_invoke([]() -> RawResult { co_return serde_raw{"null"}; });
    }
    ast.escalate(*session);
    return kota::co_invoke([this,
                            ticket = Ticket::take(std::move(session)),
                            pause = pump.scoped_pause(),
                            &position,
                            selected = std::move(selected),
                            token = std::move(token)]() -> RawResult {
        co_return co_await dispatcher.signature_help(ticket, position, selected, token);
    });
}

Requests::RawResult Requests::formatting(std::shared_ptr<Session> session,
                                         std::optional<protocol::Range> range,
                                         kota::cancellation_token token) {
    return kota::co_invoke([this,
                            ticket = Ticket::take(std::move(session)),
                            pause = pump.scoped_pause(),
                            range,
                            token = std::move(token)]() -> RawResult {
        auto result = co_await dispatcher.format(ticket, range, token).or_fail();
        if(!result) {
            co_return serde_raw{"null"};
        }
        if(!result->error.empty()) {
            co_await kota::fail(request_failed(std::move(result->error)));
        }
        co_return to_raw(
            feature::to_protocol(result->edits, ticket.session->positions(), encoding));
    });
}

Requests::RawResult Requests::references(Ticket ticket,
                                         Fid path_id,
                                         const protocol::Position& position,
                                         bool include_declaration) {
    co_return co_await navigate(ticket,
                                path_id,
                                position,
                                "[]",
                                [&](const Cursor& cursor) -> Reply {
                                    return to_raw(feature::to_locations(
                                        feature::references(peer_queries(),
                                                            cursor,
                                                            include_declaration,
                                                            project.config.references.limit.value),
                                        encoding));
                                });
}

Requests::RawResult Requests::document_highlights(Ticket ticket,
                                                  Fid path_id,
                                                  const protocol::Position& position) {
    co_return co_await navigate(
        ticket,
        path_id,
        position,
        "[]",
        [&](const Cursor& cursor) -> Reply {
            return to_raw(
                feature::to_protocol(feature::document_highlights(query, cursor), encoding));
        });
}

Requests::RawResult Requests::prepare_rename(Ticket ticket,
                                             Fid path_id,
                                             const protocol::Position& position) {
    // Refused up front, before the cursor costs a compile.
    if(auto refused = feature::workspace_refusal(project.config.workspace_root)) {
        co_await kota::fail(request_failed(std::move(*refused)));
    }
    co_return co_await navigate(ticket,
                                path_id,
                                position,
                                "null",
                                [&](const Cursor& cursor) -> Reply {
                                    auto renamed = feature::rename_at(query, cursor);
                                    if(!renamed) {
                                        return std::unexpected(
                                            request_failed(std::move(renamed.error())));
                                    }
                                    return to_raw(protocol::PrepareRenamePlaceholder{
                                        .range = feature::to_range(renamed->token, encoding),
                                        .placeholder = renamed->target.symbol.symbol.name,
                                    });
                                });
}

kota::task<std::optional<feature::Renamed>, ipc::Error>
    Requests::rename(Ticket ticket,
                     Fid path_id,
                     const protocol::Position& position,
                     std::string new_name) {
    // Refused up front, before the cursor costs a compile.
    if(auto refused = feature::workspace_refusal(project.config.workspace_root)) {
        co_await kota::fail(request_failed(std::move(*refused)));
    }
    if(ticket.session) {
        if(auto stop = co_await nav_gate(ticket)) {
            if(stop->error) {
                co_await kota::fail(std::move(*stop->error));
            }
            co_return std::nullopt;
        }
    }
    auto& config = project.config;
    auto cursor = cursor_at(path_id, position);
    if(!cursor) {
        co_return std::nullopt;
    }
    auto at = feature::rename_at(query, *cursor);
    if(!at) {
        co_await kota::fail(request_failed(std::move(at.error())));
    }

    // The walk, the reads and the scan run off the loop; only the files
    // spelling a name come back with their text.
    CanonicalPath cache_dir;
    if(!config.project.cache_dir.empty()) {
        cache_dir = CanonicalPath(Spelling::absolute(config.project.cache_dir));
    }
    auto root = config.workspace_root;
    auto texts = co_await kota::queue(
        [root, cache_dir, names = llvm::StringSet<>{at->target.symbol.symbol.name, new_name}] {
            std::vector<std::string> files;
            for(auto& source: workspace_sources(root, cache_dir)) {
                files.push_back(std::move(source).str());
            }
            return feature::WorkspaceTexts(std::move(files), names);
        });
    if(ticket.session && !ticket.fresh()) {
        co_await kota::fail(content_modified());
    }
    // Entries indexed during the scan may link the symbol to more: plan
    // with the group they give now.
    cursor = cursor_at(path_id, position);
    if(!cursor) {
        co_await kota::fail(content_modified());
    }
    auto old_name = std::move(at->target.symbol.symbol.name);
    at = feature::rename_at(query, *cursor);
    if(!at) {
        co_await kota::fail(request_failed(std::move(at.error())));
    }
    if(at->target.symbol.symbol.name != old_name) {
        co_await kota::fail(content_modified());
    }

    auto editable = [&](llvm::StringRef path) {
        return workspace_file(root, cache_dir, CanonicalPath(Spelling::absolute(path)));
    };
    // Open buffers are read as the editor holds them.
    auto buffer = [&](llvm::StringRef path) -> std::optional<llvm::StringRef> {
        auto file = project.file_table.find(Spelling::absolute(path));
        auto document = file ? sessions.find(*file) : nullptr;
        return document ? std::optional(document->text()) : std::nullopt;
    };
    auto read = [&](llvm::StringRef path) {
        return texts.read(path, buffer);
    };
    auto plan = feature::plan_rename(query,
                                     project.file_table,
                                     at->target,
                                     new_name,
                                     {.files = texts.files,
                                      .editable = editable,
                                      .read = read,
                                      .units_pending = clice::query::units_pending(project)});
    if(plan.blocked()) {
        co_await kota::fail(request_failed(feature::refusal(plan)));
    }
    co_return feature::to_protocol(
        plan,
        new_name,
        [&](Fid file) -> std::optional<int> {
            auto document = sessions.find(file);
            return document ? std::optional(document->version) : std::nullopt;
        },
        encoding);
}

Requests::RawResult Requests::declaration(Ticket ticket,
                                          Fid path_id,
                                          const protocol::Position& position) {
    co_return co_await navigate(
        ticket,
        path_id,
        position,
        "[]",
        [&](const Cursor& cursor) -> Reply {
            return to_raw(
                feature::to_locations(feature::declaration(peer_queries(), cursor), encoding));
        });
}

Requests::RawResult Requests::type_definition(Ticket ticket,
                                              Fid path_id,
                                              const protocol::Position& position) {
    co_return co_await navigate(
        ticket,
        path_id,
        position,
        "[]",
        [&](const Cursor& cursor) -> Reply {
            return to_raw(
                feature::to_locations(feature::type_definition(peer_queries(), cursor), encoding));
        });
}

Requests::RawResult Requests::implementation(Ticket ticket,
                                             Fid path_id,
                                             const protocol::Position& position) {
    co_return co_await navigate(
        ticket,
        path_id,
        position,
        "[]",
        [&](const Cursor& cursor) -> Reply {
            return to_raw(
                feature::to_locations(feature::implementation(peer_queries(), cursor), encoding));
        });
}

Requests::RawResult Requests::call_hierarchy_prepare(Ticket ticket,
                                                     Fid path_id,
                                                     const protocol::Position& position) {
    co_return co_await navigate(
        ticket,
        path_id,
        position,
        "null",
        [&](const Cursor& cursor) -> Reply {
            auto items = feature::call_hierarchy_items(peer_queries(), cursor, encoding);
            if(items.empty()) {
                return serde_raw{"null"};
            }
            return to_raw(items);
        });
}

Requests::RawResult Requests::call_hierarchy_incoming(Fid path_id,
                                                      const protocol::CallHierarchyItem& item) {
    auto symbol = feature::item_symbol(query,
                                       path_id,
                                       cursor_at(path_id, item.selection_range.start),
                                       item.data);
    if(!symbol)
        co_await kota::fail(item_not_resolved("call hierarchy"));
    co_return to_raw(feature::incoming_calls(peer_queries(), *symbol, path_id, encoding));
}

Requests::RawResult Requests::call_hierarchy_outgoing(Fid path_id,
                                                      const protocol::CallHierarchyItem& item) {
    auto symbol = feature::item_symbol(query,
                                       path_id,
                                       cursor_at(path_id, item.selection_range.start),
                                       item.data);
    if(!symbol)
        co_await kota::fail(item_not_resolved("call hierarchy"));
    co_return to_raw(feature::outgoing_calls(peer_queries(), *symbol, path_id, encoding));
}

Requests::RawResult Requests::type_hierarchy_prepare(Ticket ticket,
                                                     Fid path_id,
                                                     const protocol::Position& position) {
    co_return co_await navigate(
        ticket,
        path_id,
        position,
        "null",
        [&](const Cursor& cursor) -> Reply {
            auto items = feature::type_hierarchy_items(peer_queries(), cursor, encoding);
            if(items.empty()) {
                return serde_raw{"null"};
            }
            return to_raw(items);
        });
}

Requests::RawResult Requests::type_hierarchy_supertypes(Fid path_id,
                                                        const protocol::TypeHierarchyItem& item) {
    auto symbol = feature::item_symbol(query,
                                       path_id,
                                       cursor_at(path_id, item.selection_range.start),
                                       item.data);
    if(!symbol)
        co_await kota::fail(item_not_resolved("type hierarchy"));
    co_return to_raw(feature::supertypes(peer_queries(), *symbol, path_id, encoding));
}

Requests::RawResult Requests::type_hierarchy_subtypes(Fid path_id,
                                                      const protocol::TypeHierarchyItem& item) {
    auto symbol = feature::item_symbol(query,
                                       path_id,
                                       cursor_at(path_id, item.selection_range.start),
                                       item.data);
    if(!symbol)
        co_await kota::fail(item_not_resolved("type hierarchy"));
    co_return to_raw(feature::subtypes(peer_queries(), *symbol, path_id, encoding));
}

std::vector<feature::WorkspaceSymbol> Requests::workspace_symbols(llvm::StringRef text) {
    return feature::workspace_symbols(peer_queries(),
                                      text,
                                      Spelling(project.config.workspace_root),
                                      encoding);
}

}  // namespace clice

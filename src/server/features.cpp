module;

#include "modules/prelude.h"

module clice;

import :command.search_config;
import :index.rename;
import :project.command_resolver;
import :project.hosting;
import :sched.index.pump;
import :semantic.symbol;
import :server.ast_family;
import :server.editor_context;
import :server.features;
import :server.format;
import :server.lsp_projection;
import :server.query_commands;
import :syntax.completion;
import :syntax.include_resolver;
import :syntax.scan;
import :vfs.dir_cache;
import :vfs.file_system;
import :worker.protocol;
import :worker.serialize;

namespace clice {

using serde_raw = kota::codec::RawValue;

/// How the user knows a file a worker names by its identity.
static std::string shown(FileTable& files, llvm::StringRef identity) {
    return files.display(files.intern(Spelling::absolute(identity)));
}

/// The link whose argument covers `offset`. Link ranges are half-open;
/// contains() would also accept end.
const static index::DocumentLink* link_at(llvm::ArrayRef<index::DocumentLink> links,
                                          std::uint32_t offset) {
    auto it = llvm::find_if(links, [&](const index::DocumentLink& link) {
        return offset >= link.range.begin && offset < link.range.end;
    });
    return it != links.end() ? &*it : nullptr;
}

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
    return ast.projections.index_current(session.path_id) && !ASTFamily::compile_barred(session);
}

kota::task<Features::Route> Features::pick_route(const Ticket& ticket,
                                                 RouteOptions options,
                                                 std::optional<index::RowSource>* source) {
    for(bool waited = false;;) {
        if(!ticket.fresh()) {
            co_return Route::Superseded;
        }
        auto& session = *ticket.session;
        if(ast_answerable(session)) {
            co_return Route::Ast;
        }
        // An oversized buffer is not worth a synchronous main-thread lex;
        // the full-lex projections follow the investment policy instead of
        // the index slice. Row-backed answers serve at any size.
        constexpr std::size_t full_lex_cap = 8 * 1024 * 1024;
        bool capped = options.full_lex && session.text.size() > full_lex_cap;
        if(!capped) {
            // The session's own rows when current (the quarantine fallback
            // — the worker cannot be asked, the rows are still true), else
            // the disk shard admitted by freshness clause 4.
            if(auto serving = query.serving(session.path_id)) {
                // An index answer must not strand an escalated session's
                // pending build: this request still pulls the compile it
                // would otherwise have waited on, just without blocking.
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

Features::RawResult Features::stop_reply(Stop stop) {
    if(stop.error) {
        co_await kota::fail(std::move(*stop.error));
    }
    co_return serde_raw{"null"};
}

kota::task<std::optional<Features::Stop>> Features::nav_gate(const Ticket& ticket) {
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

std::optional<index::IndexQuery::Cursor>
    Features::cursor_at(Fid path_id, const protocol::Position& position) const {
    // A document that names no file (an `untitled:` buffer) has no place
    // in the index.
    if(!path_id.valid()) {
        return std::nullopt;
    }
    return query.symbol_at(path_id, position.line, position.character);
}

/// The command a file compiles with — among a multi-configuration
/// file's or host's entries the pinned one, else the first, or the default
/// command claiming it — rules applied, like the resolve path's effective
/// command: whether it compiles the file as C, whether an -x forces that
/// (the driver override beats every suffix heuristic), and the last -std
/// value.
struct CommandLang {
    CommandSource source;
    bool is_c = false;
    bool language_forced = false;
    std::string standard;
};

static std::optional<CommandLang> command_lang(Project& project,
                                               Fid file,
                                               llvm::ArrayRef<CanonicalRef> paths,
                                               const Selection* pin) {
    auto path = project.file_table.resolve(file);
    auto commands = project.build.commands(file);
    if(commands.empty()) {
        return std::nullopt;
    }
    auto command = pick_pinned_config(project,
                                      file,
                                      commands,
                                      paths,
                                      path,
                                      pin ? llvm::StringRef(pin->command_hash) : "",
                                      pin ? llvm::StringRef(pin->base_hash) : "");
    auto ref = project.build.resolve(file, command.config, command.source, paths, path);

    llvm::StringRef language = ref.input.value;
    CommandLang result{
        .source = command.source,
        .is_c = language == "c" || language == "c-header",
        .language_forced = !project.cdb.forced_language(ref.config).empty(),
    };
    for(auto& arg: project.cdb.config(ref.config).args) {
        if(arg.opt_id == option::OPT_std_EQ && arg.values.size() == 1) {
            result.standard = arg.values[0];
        }
    }
    return result;
}

Fid Features::host_of(Fid path_id) const {
    if(auto it = contexts.selections.find(path_id); it != contexts.selections.end()) {
        if(it->second.host_path_id.valid()) {
            return it->second.host_path_id;
        }
    }
    if(const auto* context = contexts.header_context(path_id)) {
        return context->host_path_id;
    }
    // A header compiled under its own entry resolves no context; its
    // includers still name the source its definitions belong in.
    auto hosts = ranked_hosts(project, path_id);
    return hosts.empty() ? Fid{} : hosts.front();
}

const clang::LangOptions& Features::index_lang_options(const Session& session) {
    auto path = project.file_table.resolve(session.path_id);
    const auto* pin = contexts.selection(session.path_id);
    bool host_pin = pin && pin->host_path_id.valid();
    auto own = command_lang(project, session.path_id, path, host_pin ? nullptr : pin);
    // A file's entry is its command; a default command yields to the host
    // a header borrows from, in resolve_command's order.
    if(own && own->language_forced && own->source == CommandSource::CDBExact) {
        return feature::index_lang_options("", own->is_c, own->standard);
    }

    // A header's active context (the user's persisted choice, else the
    // resolved host) names the view being read; its command beats the
    // contributor union the way it does for the AST after an escalation.
    if(Fid host = host_of(session.path_id); host.valid()) {
        CanonicalRef edit_paths[] = {project.file_table.resolve(host), path};
        auto host_lang = command_lang(project, host, edit_paths, host_pin ? pin : nullptr);
        if(host_lang) {
            return feature::index_lang_options("", host_lang->is_c, host_lang->standard);
        }
    }

    if(own && own->language_forced) {
        return feature::index_lang_options("", own->is_c, own->standard);
    }

    auto& contributions = project.project_index.contributions;
    auto it = contributions.find(session.path_id);
    bool c_rows = it != contributions.end() && !it->second.empty() &&
                  llvm::all_of(llvm::make_first_range(it->second), [&](Fid tu) {
                      return llvm::StringRef(project.file_table.resolve(tu)).ends_with(".c");
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
    auto symbol = cursor->symbols.front();
    auto info = query.symbol_info(symbol, cursor->site.file);
    if(!info) {
        return std::nullopt;
    }
    if(info->kind == SymbolKind::Module) {
        return module_hover_card(*cursor, *info);
    }
    std::string definition;
    std::string comment;
    if(auto text = query.definition_text(symbol, cursor->site.file)) {
        definition = std::move(text->text);
        comment = std::move(text->comment);
    }
    auto hover = feature::index_hover(*info, definition, comment);
    hover.symbol_range = cursor->site.range;
    return hover;
}

feature::HoverInfo Features::module_hover_card(const index::IndexQuery::Cursor& cursor,
                                               const index::SymbolRef& module) {
    feature::HoverInfo hover;
    hover.name = module.name;
    hover.kind = SymbolKind::Module;
    auto units = gather(module.hash,
                        cursor.site.file,
                        [&](const index::IndexQuery& from, index::SymbolHash named) {
                            return from.sites(named, cursor.site.file, RelationKind::Definition);
                        });
    if(!units.empty()) {
        hover.definition = shown(project.file_table, units.front().path);
    }
    hover.symbol_range = cursor.site.range;
    return hover;
}

std::vector<index::DocumentLink> Features::find_preamble_links(const Session& session) {
    auto state = query.preamble_blob(session.path_id);
    return state ? state->links() : std::vector<index::DocumentLink>{};
}

std::vector<protocol::Location> Features::directive_definition(const index::DocumentLink& link) {
    return {
        protocol::Location{
                           .uri = feature::to_uri(shown(project.file_table, link.target)),
                           .range = protocol::Range{},
                           }
    };
}

std::optional<protocol::Hover> Features::directive_hover(const Session& session,
                                                         const index::DocumentLink& link) {
    if(link.range.end > session.text.size()) {
        return std::nullopt;
    }
    llvm::StringRef name(session.text.data() + link.range.begin, link.range.length());
    name = name.trim();
    if(name.size() >= 2 && ((name.front() == '"' && name.back() == '"') ||
                            (name.front() == '<' && name.back() == '>'))) {
        name = name.drop_front().drop_back();
    }

    feature::HoverInfo info;
    info.name = name.str();
    info.kind = SymbolKind::Header;
    info.definition = shown(project.file_table, link.target);
    info.symbol_range = link.range;

    auto hover = feature::to_protocol_hover(info, project.config.hover, session.position_map());
    if(!hover.range) {
        return std::nullopt;
    }
    return hover;
}

kota::task<std::vector<index::DocumentLink>, kota::ipc::Error>
    Features::directive_links(const Ticket& ticket, kota::cancellation_token token) {
    auto result = co_await dispatcher.document_links(ticket, std::move(token)).or_fail();
    // The preamble is compiled into the PCH, so the worker's AST only
    // covers the rest of the file — merge the preamble's links in front.
    auto links = find_preamble_links(*ticket.session);
    links.insert(links.end(), result.begin(), result.end());
    co_return links;
}

kota::task<std::vector<protocol::DocumentLink>, kota::ipc::Error>
    Features::document_links(Ticket ticket, kota::cancellation_token token) {
    auto& session = ticket.session;

    // Links carry byte offsets; this reply edge converts them.
    auto convert = [&](llvm::ArrayRef<index::DocumentLink> raw_links,
                       std::vector<protocol::DocumentLink>& links) {
        auto map = session->position_map();
        for(const auto& link: raw_links) {
            auto range = map.to_range(link.range);
            if(!range)
                continue;
            protocol::DocumentLink out{.range = *range};
            auto path = shown(project.file_table, link.target);
            out.target = feature::to_uri(path);
            out.tooltip = std::move(path);
            links.push_back(std::move(out));
        }
    };

    switch(co_await pick_route(ticket, {.await_cold_attempt = true})) {
        case Route::Superseded: co_await kota::fail(content_modified());
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
            if(query.shard_matching(session->path_id, session->text)) {
                auto raw = feature::index_document_links(session->text,
                                                         index_lang_options(*session),
                                                         query.include_edges(session->path_id));
                convert(raw, links);
            }
            session->index_served = true;
            co_return links;
        }
        case Route::Empty: co_return std::vector<protocol::DocumentLink>{};
        case Route::Ast: break;
    }

    auto result = co_await directive_links(ticket, std::move(token)).or_fail();

    std::vector<protocol::DocumentLink> links;
    convert(result, links);
    co_return links;
}

kota::task<std::vector<protocol::Diagnostic>>
    Features::diagnostics(std::shared_ptr<Session> session) {
    while(session->serving == ServingMode::Escalated && !session->closed) {
        auto ticket = Ticket::take(session);
        co_await ast.ensure_compiled(session);
        if(ticket.fresh()) {
            break;
        }
    }
    co_return settled_diagnostics(*session);
}

std::vector<protocol::Diagnostic> Features::settled_diagnostics(const Session& session) const {
    std::vector<protocol::Diagnostic> diagnostics;
    if(session.closed) {
        return diagnostics;
    }
    // A compile that failed or is barred left an output of an older
    // buffer, whose ranges a versionless report would place on this one.
    if(auto projection = ast.projections.projection_at(session.path_id, session.version)) {
        diagnostics = format_diagnostics(*projection->output);
    }
    append_crash_notes(session, diagnostics);
    return diagnostics;
}

Features::RawResult Features::definition(Ticket ticket,
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
    // the index and are invisible to the worker's AST. A projection still
    // not current after the awaited compile means the world was re-dirtied
    // mid-flight (the round landed as bounded staleness): the cached
    // links may describe a pre-edit preamble — skip, and let the index and
    // worker paths below answer.
    auto offset = session ? session->position_map().to_offset(position) : std::nullopt;
    if(offset && ast.projections.current(path_id)) {
        auto links = find_preamble_links(*session);
        if(auto* link = link_at(links, *offset)) {
            co_return to_raw(directive_definition(*link));
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
        // Each symbol answers for itself. A definition another project
        // compiles (a library's source beside the application including
        // its header) outranks the declarations this project alone can
        // offer; standing on a definition, or with none anywhere, the
        // declarations answer too.
        std::vector<index::Site> result;
        for(auto symbol: cursor->symbols) {
            auto defined = gather(symbol,
                                  path_id,
                                  [&](const index::IndexQuery& from, index::SymbolHash named) {
                                      return from.sites(named, path_id, RelationKind::Definition);
                                  });
            if(defined.empty() || llvm::any_of(defined, [&](const index::Site& site) {
                   return index::covers(site, cursor->site);
               })) {
                defined =
                    gather(symbol,
                           path_id,
                           [&](const index::IndexQuery& from, index::SymbolHash named) {
                               return from.definition({.symbols = {named}, .site = cursor->site});
                           });
            }
            llvm::append_range(result, std::move(defined));
        }
        index::dedup_sites(result);
        return to_lsp::locations(result);
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
    // content gate as the links projection: manifest lines are meaningless
    // against a buffer the index never described.
    auto serving = query.serving(path_id);
    if(session->serving == ServingMode::IndexOnly || ASTFamily::compile_barred(*session) ||
       (serving && serving->kind == index::RowSource::Kind::Shard)) {
        if(!query.shard_matching(session->path_id, session->text)) {
            co_return serde_raw{"[]"};
        }
        if(offset) {
            auto links = feature::index_document_links(session->text,
                                                       index_lang_options(*session),
                                                       query.include_edges(session->path_id));
            if(auto* link = link_at(links, *offset)) {
                co_return to_raw(directive_definition(*link));
            }
        }
        co_return serde_raw{"[]"};
    }

    // A dispatch error is final: a ContentModified in particular must not
    // be replaced by an index answer computed on the newer buffer.
    auto links = co_await directive_links(ticket, std::move(token)).or_fail();
    if(auto* link = offset ? link_at(links, *offset) : nullptr) {
        co_return to_raw(directive_definition(*link));
    }

    // The dispatch compiled a dirty buffer: retry against the refreshed
    // projection, but only when the compile actually completed — a failed
    // compile leaves the projection non-current and the caches stale.
    if(ast.projections.current(path_id)) {
        if(auto retry = index_definition(); !retry.empty()) {
            co_return to_raw(retry);
        }
    }
    co_return serde_raw{"[]"};
}

Features::RawResult Features::hover(Ticket ticket,
                                    const protocol::Position& position,
                                    kota::cancellation_token token) {
    auto& session = ticket.session;
    if(!session) {
        co_await kota::fail(document_not_open());
    }
    auto path_id = session->path_id;

    auto index_card = [&]() -> std::optional<serde_raw> {
        if(auto info = index_hover_card(*session, position)) {
            return to_raw(
                feature::to_protocol_hover(*info, project.config.hover, session->position_map()));
        }
        return std::nullopt;
    };

    switch(co_await pick_route(ticket, {})) {
        case Route::Superseded: co_await kota::fail(content_modified());
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

    // A directive's card names its target, which the worker knows only by
    // identity: this side answers it, from the links.
    auto offset = session->position_map().to_offset(position);
    auto argument = offset ? feature::find_directive_argument(session->text,
                                                              *offset,
                                                              &index_lang_options(*session))
                           : std::nullopt;
    if(argument && argument->begin <= *offset) {
        auto links = co_await directive_links(ticket, token).or_fail();
        if(auto* link = link_at(links, *offset)) {
            auto hover = directive_hover(*session, *link);
            co_return hover ? to_raw(*hover) : serde_raw{"null"};
        }
    }

    // A module name's card names the unit defining the module, which the
    // worker cannot see across files: this side answers it, from the index.
    if(auto cursor = cursor_at(path_id, position)) {
        auto info = query.symbol_info(cursor->symbols.front(), cursor->site.file);
        if(info && info->kind == SymbolKind::Module) {
            co_return to_raw(feature::to_protocol_hover(module_hover_card(*cursor, *info),
                                                        project.config.hover,
                                                        session->position_map()));
        }
    }

    auto raw =
        co_await dispatcher.query(worker::QueryKind::Hover, ticket, position, {}, std::move(token));
    if(raw.has_value() && to_lsp::is_null(raw.value()) && ast.projections.current(path_id)) {
        // The preamble region is the one place a null from the AST is not
        // authoritative — it is compiled into the PCH and invisible to
        // the worker (a `#define` there has no node). The index card
        // fills that gap, and only that gap: past the bound the AST saw
        // the code and declined on purpose (a lambda's auto parameter
        // must not hover, and the index would happily name it `auto:1`).
        auto projection = ast.projections.projection(path_id);
        if(projection && projection->pch_key) {
            if(auto it = project.pch_cache.find(*projection->pch_key);
               it != project.pch_cache.end()) {
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

Features::RawResult Features::semantic_tokens(Ticket ticket, kota::cancellation_token token) {
    auto& session = ticket.session;
    std::optional<index::RowSource> source;
    switch(co_await pick_route(ticket, {.full_lex = true}, &source)) {
        case Route::Superseded: co_await kota::fail(content_modified());
        case Route::Index: {
            auto rows = feature::extract_index_rows(*source->rows);
            auto tokens = feature::index_semantic_tokens(
                session->text,
                index_lang_options(*session),
                rows.occurrences,
                rows.decls,
                [&](index::SymbolHash hash) { return query.symbol_info(hash); });
            session->index_served = true;
            co_return to_raw(feature::semantic_tokens_to_protocol(tokens, session->position_map()));
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

Features::RawResult Features::inlay_hints(Ticket ticket,
                                          const protocol::Range& range,
                                          kota::cancellation_token token) {
    auto& session = ticket.session;
    // Inlay hints are Sema products the index cannot project; a session
    // the policy keeps un-compiled answers honestly empty. The compile
    // that follows an escalation pushes an inlayHint refresh, so clients
    // re-pull once real hints exist.
    if(session->serving == ServingMode::IndexOnly) {
        session->index_served = true;
        co_return serde_raw{"[]"};
    }
    co_return co_await dispatcher.query(worker::QueryKind::InlayHints,
                                        ticket,
                                        {},
                                        range,
                                        std::move(token));
}

Features::RawResult Features::folding_range(Ticket ticket,
                                            bool line_folding_only,
                                            kota::cancellation_token token) {
    auto& session = ticket.session;
    auto convert = [&](llvm::ArrayRef<feature::FoldingRange> folds) {
        return to_raw(
            feature::folding_ranges_to_protocol(folds, session->position_map(), line_folding_only));
    };

    std::optional<index::RowSource> source;
    switch(co_await pick_route(ticket, {.full_lex = true}, &source)) {
        case Route::Superseded: co_await kota::fail(content_modified());
        case Route::Index: {
            auto rows = feature::extract_index_rows(*source->rows);
            auto folds = feature::index_folding_ranges(
                session->text,
                index_lang_options(*session),
                rows.decls,
                [&](index::SymbolHash hash) { return query.symbol_info(hash); });
            session->index_served = true;
            co_return convert(folds);
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
    auto folds = co_await dispatcher.folding_ranges(ticket, std::move(token)).or_fail();
    if(!folds) {
        co_return serde_raw{"null"};
    }
    co_return convert(*folds);
}

Features::RawResult Features::document_symbol(Ticket ticket, kota::cancellation_token token) {
    auto& session = ticket.session;
    std::optional<index::RowSource> source;
    switch(co_await pick_route(ticket, {.await_cold_attempt = true}, &source)) {
        case Route::Superseded: co_await kota::fail(content_modified());
        case Route::Index: {
            auto rows = feature::extract_index_rows(*source->rows);
            auto symbols = feature::index_document_symbols(rows.decls, [&](index::SymbolHash hash) {
                return query.symbol_info(hash);
            });
            session->index_served = true;
            co_return to_raw(
                feature::document_symbols_to_protocol(symbols, session->position_map()));
        }
        case Route::Empty: co_return serde_raw{"[]"};
        case Route::Ast: break;
    }
    co_return co_await dispatcher.query(worker::QueryKind::DocumentSymbol,
                                        ticket,
                                        {},
                                        {},
                                        std::move(token));
}

Features::RawResult Features::completion(std::shared_ptr<Session> session,
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

Features::RawResult Features::complete(std::shared_ptr<Session> session,
                                       IndexPump::ScopedPause pause,
                                       const protocol::Position& position,
                                       const feature::CompletionClient& client,
                                       llvm::StringRef trigger_character,
                                       kota::cancellation_token token) {
    // The task starts once the messages read with the request are
    // dispatched: a $/cancelRequest among them (rapid-fire completions
    // cancel and re-issue as the user types) keeps it from starting, and an
    // edit lands before the offset and completion context are computed,
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

    auto map = session->position_map();
    auto offset = map.to_offset(position);

    PreambleCompletionContext pctx;
    if(offset) {
        pctx = detect_completion_context(session->text, *offset);
    }

    // Clients without request-side gating (nvim, zed) forward every
    // keystroke of an advertised trigger character. Space is advertised
    // only so that `import ` opens module suggestions; the others exist
    // for include paths and member or scope access. Anything else —
    // `template<`, a closing `>`, the dots of a pack ellipsis — is
    // answered with an empty list before any include scanning or
    // completion build.
    if(!trigger_character.empty()) {
        bool served = trigger_character == " "
                          ? pctx.kind == CompletionContext::Import
                          : pctx.kind != CompletionContext::None || !offset ||
                                follows_access_operator(session->text, *offset);
        if(!served) {
            co_return serde_raw{"[]"};
        }
    }

    if(offset) {
        if(pctx.kind == CompletionContext::IncludeQuoted ||
           pctx.kind == CompletionContext::IncludeAngled) {
            std::string directory;
            std::vector<std::string> arguments;
            // Editor use: candidates must come from the same command (host
            // choice, chosen CDB entry) the open buffer compiles under.
            auto ref = contexts.resolve_command(path_id, directory, arguments).ref;

            auto search_config = project.cdb.search_config(ref);
            vfs::Scope scope(project.file_table.dirs);
            bool angled = (pctx.kind == CompletionContext::IncludeAngled);
            auto candidates = complete_include_path(search_config,
                                                    path::parent_path(path),
                                                    pctx.prefix,
                                                    angled,
                                                    scope);

            // A directory continues the path, replacing a `/` already
            // there; a header ends it and closes the directive, replacing
            // the rest of the path through a closing delimiter already there.
            llvm::StringRef text = session->text;
            char closer = angled ? '>' : '"';
            auto line_end = std::min(text.find_first_of("\r\n", pctx.replace.end), text.size());
            auto close = text.slice(pctx.replace.end, line_end).find(closer);
            auto edit = [&](const IncludeCandidate& candidate) {
                auto end = pctx.replace.end;
                if(candidate.is_directory) {
                    if(text.substr(end).starts_with("/")) {
                        end += 1;
                    }
                } else if(close != llvm::StringRef::npos) {
                    end += close + 1;
                }
                return protocol::TextEdit{
                    .range = *map.to_range({pctx.replace.begin, end}),
                    .new_text = candidate.name + (candidate.is_directory ? '/' : closer),
                };
            };
            std::vector<protocol::CompletionItem> items;
            items.reserve(candidates.size());
            for(auto& c: candidates) {
                protocol::CompletionItem item;
                item.label = c.is_directory ? c.name + "/" : c.name;
                item.kind = protocol::CompletionItemKind::File;
                item.text_edit = edit(c);
                items.push_back(std::move(item));
            }
            co_return to_raw(items);
        }
        if(pctx.kind == CompletionContext::Import) {
            llvm::StringRef text = session->text;
            auto module_names = complete_module_import(project.dep_graph,
                                                       pctx.prefix,
                                                       scan_quick(text).module_name);

            // A statement closed already keeps its semicolon.
            auto line_end = std::min(text.find_first_of("\r\n", pctx.replace.end), text.size());
            bool closed = text.slice(pctx.replace.end, line_end).contains(';');
            std::vector<protocol::CompletionItem> items;
            items.reserve(module_names.size());
            for(auto& name: module_names) {
                protocol::CompletionItem item;
                item.label = name;
                item.kind = protocol::CompletionItemKind::Module;
                item.text_edit = protocol::TextEdit{
                    .range = *map.to_range(pctx.replace),
                    .new_text = closed ? name : name + ";",
                };
                items.push_back(std::move(item));
            }
            co_return to_raw(items);
        }
    }

    co_return co_await dispatcher.completion(ticket, position, client, std::move(token));
}

Features::RawResult Features::signature_help(std::shared_ptr<Session> session,
                                             const protocol::Position& position,
                                             kota::cancellation_token token) {
    ast.escalate(*session);
    return kota::co_invoke([this,
                            ticket = Ticket::take(std::move(session)),
                            pause = pump.scoped_pause(),
                            &position,
                            token = std::move(token)]() -> RawResult {
        co_return co_await dispatcher.signature_help(ticket, position, token);
    });
}

Features::RawResult Features::formatting(std::shared_ptr<Session> session,
                                         std::optional<protocol::Range> range,
                                         kota::cancellation_token token) {
    return kota::co_invoke([this,
                            ticket = Ticket::take(std::move(session)),
                            pause = pump.scoped_pause(),
                            range,
                            token = std::move(token)]() -> RawResult {
        co_return co_await dispatcher.format(ticket, range, token);
    });
}

Features::RawResult Features::references(Ticket ticket,
                                         Fid path_id,
                                         const protocol::Position& position,
                                         bool include_declaration) {
    if(ticket.session) {
        if(auto stop = co_await nav_gate(ticket)) {
            co_return co_await stop_reply(std::move(*stop));
        }
    }
    auto cursor = cursor_at(path_id, position);
    if(!cursor) {
        co_return serde_raw{"[]"};
    }
    co_return to_raw(to_lsp::locations(gather(
        cursor->symbols,
        path_id,
        [&](const index::IndexQuery& from, index::SymbolHash named) {
            return from.references({.symbols = {named}, .site = cursor->site}, include_declaration);
        })));
}

static kota::ipc::Error rename_refused(std::string message) {
    return kota::ipc::Error{static_cast<protocol::integer>(protocol::LSPErrorCodes::RequestFailed),
                            std::move(message)};
}

/// Refused up front, before the cursor costs a compile.
static std::optional<kota::ipc::Error> refuse_rootless(const Project& project) {
    if(project.config.workspace_root.empty()) {
        return rename_refused("a rename edits the sources of a workspace folder; open one");
    }
    return std::nullopt;
}

/// `title` and up to a handful of `items`, one a line.
static void list_notice(std::string& notice,
                        std::string_view title,
                        llvm::ArrayRef<std::string> items) {
    constexpr std::size_t shown_items = 5;
    if(items.empty()) {
        return;
    }
    notice += std::format("{}{}:", notice.empty() ? "" : "\n", title);
    for(auto& item: items.take_front(shown_items)) {
        notice += std::format("\n  {}", item);
    }
    if(items.size() > shown_items) {
        notice += std::format("\n  and {} more", items.size() - shown_items);
    }
}

Features::RawResult Features::prepare_rename(Ticket ticket,
                                             Fid path_id,
                                             const protocol::Position& position) {
    if(auto refused = refuse_rootless(project)) {
        co_await kota::fail(std::move(*refused));
    }
    if(ticket.session) {
        if(auto stop = co_await nav_gate(ticket)) {
            co_return co_await stop_reply(std::move(*stop));
        }
    }
    auto cursor = cursor_at(path_id, position);
    if(!cursor) {
        co_return serde_raw{"null"};
    }
    auto renamed = index::rename_at(query, *cursor);
    if(!renamed) {
        co_await kota::fail(rename_refused(std::move(renamed.error())));
    }
    co_return to_raw(protocol::PrepareRenamePlaceholder{
        .range = to_lsp::range(renamed->token),
        .placeholder = renamed->target.symbol.symbol.name,
    });
}

kota::task<std::optional<Features::Renamed>, kota::ipc::Error>
    Features::rename(Ticket ticket,
                     Fid path_id,
                     const protocol::Position& position,
                     std::string new_name) {
    if(auto refused = refuse_rootless(project)) {
        co_await kota::fail(std::move(*refused));
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
    auto at = index::rename_at(query, *cursor);
    if(!at) {
        co_await kota::fail(rename_refused(std::move(at.error())));
    }

    // The walk, the reads and the sweep run off the loop; only the files
    // spelling a name come back with their text.
    struct Swept {
        std::vector<std::string> files;
        llvm::StringMap<index::SweptText> texts;
    };

    CanonicalPath cache_dir;
    if(!config.project.cache_dir.empty()) {
        cache_dir = CanonicalPath(Spelling::absolute(config.project.cache_dir));
    }
    auto swept = co_await kota::queue([root = config.workspace_root,
                                       cache_dir,
                                       old_name = at->target.symbol.symbol.name,
                                       new_name] {
        Swept result;
        for(auto& source: workspace_sources(root, cache_dir)) {
            auto path = source.str();
            if(auto text = vfs::read(path)) {
                if(auto spelled =
                       index::sweep_text((*text)->getBuffer().str(), old_name, new_name)) {
                    result.texts.try_emplace(path, std::move(*spelled));
                }
            }
            result.files.push_back(std::move(path));
        }
        return result;
    });
    if(ticket.session && !ticket.fresh()) {
        co_await kota::fail(content_modified());
    }
    // Rows indexed during the sweep may link the symbol to more: plan
    // with the group they give now.
    cursor = cursor_at(path_id, position);
    if(!cursor) {
        co_await kota::fail(content_modified());
    }
    auto old_name = std::move(at->target.symbol.symbol.name);
    at = index::rename_at(query, *cursor);
    if(!at) {
        co_await kota::fail(rename_refused(std::move(at.error())));
    }
    if(at->target.symbol.symbol.name != old_name) {
        co_await kota::fail(content_modified());
    }

    auto root = config.workspace_root;
    llvm::StringSet<> walked;
    for(auto& path: swept.files) {
        walked.insert(path);
    }
    auto editable = [&](llvm::StringRef path) {
        return workspace_file(root, cache_dir, CanonicalPath(Spelling::absolute(path)));
    };
    auto read = [&](llvm::StringRef path) -> std::optional<index::SweptText> {
        auto file = project.file_table.find(Spelling::absolute(path));
        if(auto document = file ? sessions.find(*file) : nullptr) {
            return index::sweep_text(document->text, old_name, new_name);
        }
        if(auto it = swept.texts.find(path); it != swept.texts.end()) {
            return it->second;
        }
        // An edited file the walk's suffixes left out.
        if(!walked.contains(path)) {
            if(auto text = vfs::read(path)) {
                return index::sweep_text((*text)->getBuffer().str(), old_name, new_name);
            }
        }
        return std::nullopt;
    };
    auto plan = index::plan_rename(query,
                                   project.file_table,
                                   at->target,
                                   new_name,
                                   {.files = swept.files,
                                    .editable = editable,
                                    .read = read,
                                    .units_pending = clice::query::units_pending(project)});
    if(plan.blocked()) {
        auto reasons = plan.conflicts;
        if(!plan.stale.empty()) {
            reasons.push_back(std::format(
                "these files changed since they were indexed, or were never indexed: {}",
                llvm::join(plan.stale, ", ")));
        }
        co_await kota::fail(rename_refused(
            std::format("cannot rename `{}`: {}", plan.old_name, llvm::join(reasons, "; "))));
    }

    Renamed renamed;
    using DocumentChange = protocol::variant<protocol::TextDocumentEdit,
                                             protocol::CreateFile,
                                             protocol::RenameFile,
                                             protocol::DeleteFile>;
    std::vector<DocumentChange> changes;
    protocol::TextDocumentEdit* change = nullptr;
    std::vector<std::string> heuristic;
    for(auto& edit: plan.edits) {
        if(!change || change->text_document.uri != feature::to_uri(edit.site.path)) {
            auto document = sessions.find(edit.site.file);
            protocol::TextDocumentEdit next{
                .text_document = {.uri = feature::to_uri(edit.site.path),
                                  .version =
                                      document ? std::optional(document->version) : std::nullopt},
            };
            change = &std::get<protocol::TextDocumentEdit>(changes.emplace_back(std::move(next)));
        }
        change->edits.emplace_back(protocol::TextEdit{
            .range = to_lsp::range(edit.site),
            .new_text = new_name,
        });
        if(edit.heuristic) {
            heuristic.push_back(std::format("{}:{}", edit.site.path, edit.site.begin.line + 1));
        }
    }
    renamed.edit.document_changes = std::move(changes);

    list_notice(renamed.notice, std::format("Renaming `{}`", plan.old_name), plan.warnings);
    std::vector<std::string> left;
    for(auto& note: plan.unconfirmed) {
        left.push_back(std::format("{}:{}: {} ({})",
                                   note.site.path,
                                   note.site.begin.line + 1,
                                   note.line,
                                   note.reason));
    }
    list_notice(renamed.notice, std::format("Spellings of `{}` left alone", plan.old_name), left);
    list_notice(renamed.notice, "Renamed through a heuristic resolution", heuristic);
    co_return renamed;
}

llvm::SmallVector<Features::Source> Features::peers_of(index::SymbolHash symbol, Fid anchor) {
    llvm::SmallVector<Source> relevant;
    auto others = peers();
    if(others.empty()) {
        return relevant;
    }
    // A peer knows the same entity when it declares it in a file this
    // project declares it in: an unrelated project reusing the name (the
    // same symbol id) declares it elsewhere.
    auto declared = [&](const index::IndexQuery& from, index::SymbolHash hash) {
        std::vector<index::Site> sites;
        for(auto kind: {RelationKind::Declaration, RelationKind::Definition}) {
            llvm::append_range(sites, from.sites(hash, anchor, kind));
        }
        return sites;
    };
    auto own = declared(query, symbol);
    auto info = query.symbol_info(symbol, anchor);
    llvm::SmallVector<Fid> files{anchor};
    for(auto& site: own) {
        files.push_back(site.file);
    }
    for(auto* peer: others) {
        if(llvm::any_of(declared(*peer, symbol), [&](const index::Site& site) {
               return llvm::is_contained(files, site.file);
           })) {
            relevant.push_back({peer, symbol});
            continue;
        }
        // The same name at the same place: a file compiled under other
        // flags can hold another declaration there.
        auto same_name = [&](const index::Site& site) -> std::optional<index::SymbolHash> {
            auto cursor = peer->symbol_at(site.file, site.range.begin);
            if(!info || !cursor || !index::covers(site, cursor->site)) {
                return std::nullopt;
            }
            for(auto candidate: cursor->symbols) {
                auto named = peer->symbol_info(candidate, site.file);
                if(named && named->name == info->name && named->kind == info->kind) {
                    return candidate;
                }
            }
            return std::nullopt;
        };
        for(auto& site: own) {
            if(auto named = same_name(site)) {
                relevant.push_back({peer, *named});
                break;
            }
        }
    }
    return relevant;
}

llvm::SmallVector<Features::Source> Features::sources(index::SymbolHash symbol, Fid anchor) {
    llvm::SmallVector<Source> all{
        {&query, symbol}
    };
    llvm::append_range(all, peers_of(symbol, anchor));
    return all;
}

bool Features::answers_for(const index::IndexQuery& from,
                           Fid file,
                           llvm::ArrayRef<Source> asked) const {
    auto* serving = open_in(file);
    return !serving || serving == &from ||
           llvm::none_of(asked, [&](const Source& source) { return source.query == serving; });
}

std::vector<index::Site> Features::gather(
    llvm::ArrayRef<index::SymbolHash> symbols,
    Fid anchor,
    llvm::function_ref<std::vector<index::Site>(const index::IndexQuery&, index::SymbolHash)> ask) {
    std::vector<index::Site> sites;
    for(auto symbol: symbols) {
        auto asked = sources(symbol, anchor);
        for(auto& [from, named]: asked) {
            for(auto& site: ask(*from, named)) {
                if(answers_for(*from, site.file, asked)) {
                    sites.push_back(std::move(site));
                }
            }
        }
    }
    index::dedup_sites(sites);
    return sites;
}

/// Whether two projects' answers name one entity: the same id, or the
/// same name declared at the same place — an id hashing its file differs
/// between a project compiling the file from inside its root and one
/// compiling it from outside (Features::Source).
static bool same_entity(const index::IndexQuery::Located& lhs,
                        const index::IndexQuery::Located& rhs) {
    return lhs.symbol.hash == rhs.symbol.hash ||
           (lhs.symbol.name == rhs.symbol.name && lhs.site.file == rhs.site.file &&
            lhs.site.range == rhs.site.range);
}

/// Fold one project's graph neighbours into `into`: the sites it may
/// answer for (`keep`), one neighbour per symbol, a neighbour left with
/// no site dropped.
static void merge_edges(std::vector<index::IndexQuery::Edge>& into,
                        std::vector<index::IndexQuery::Edge> edges,
                        llvm::function_ref<bool(Fid)> keep) {
    for(auto& edge: edges) {
        llvm::erase_if(edge.sites, [&](const index::Site& site) { return !keep(site.file); });
        if(edge.sites.empty()) {
            continue;
        }
        auto known = llvm::find_if(into, [&](const index::IndexQuery::Edge& other) {
            return same_entity(other.symbol, edge.symbol);
        });
        if(known == into.end()) {
            into.push_back(std::move(edge));
        } else {
            llvm::append_range(known->sites, edge.sites);
            index::dedup_sites(known->sites);
        }
    }
}

/// The same for a type hierarchy's neighbours, placed at one site each.
static void merge_located(std::vector<index::IndexQuery::Located>& into,
                          std::vector<index::IndexQuery::Located> located,
                          llvm::function_ref<bool(Fid)> keep) {
    for(auto& one: located) {
        if(keep(one.site.file) && llvm::none_of(into, [&](const index::IndexQuery::Located& other) {
               return same_entity(other, one);
           })) {
            into.push_back(std::move(one));
        }
    }
}

Features::RawResult Features::declaration(Ticket ticket,
                                          Fid path_id,
                                          const protocol::Position& position) {
    if(ticket.session) {
        if(auto stop = co_await nav_gate(ticket)) {
            co_return co_await stop_reply(std::move(*stop));
        }
    }
    auto cursor = cursor_at(path_id, position);
    if(!cursor) {
        co_return serde_raw{"[]"};
    }
    co_return to_raw(to_lsp::locations(
        gather(cursor->symbols,
               path_id,
               [&](const index::IndexQuery& from, index::SymbolHash named) {
                   return from.declaration({.symbols = {named}, .site = cursor->site});
               })));
}

Features::RawResult Features::type_definition(Ticket ticket,
                                              Fid path_id,
                                              const protocol::Position& position) {
    if(ticket.session) {
        if(auto stop = co_await nav_gate(ticket)) {
            co_return co_await stop_reply(std::move(*stop));
        }
    }
    auto cursor = cursor_at(path_id, position);
    if(!cursor) {
        co_return serde_raw{"[]"};
    }
    co_return to_raw(to_lsp::locations(
        gather(cursor->symbols,
               path_id,
               [&](const index::IndexQuery& from, index::SymbolHash named) {
                   return from.target_sites(named, path_id, RelationKind::TypeDefinition);
               })));
}

Features::RawResult Features::implementation(Ticket ticket,
                                             Fid path_id,
                                             const protocol::Position& position) {
    if(ticket.session) {
        if(auto stop = co_await nav_gate(ticket)) {
            co_return co_await stop_reply(std::move(*stop));
        }
    }
    auto cursor = cursor_at(path_id, position);
    if(!cursor) {
        co_return serde_raw{"[]"};
    }
    co_return to_raw(
        to_lsp::locations(gather(cursor->symbols,
                                 path_id,
                                 [&](const index::IndexQuery& from, index::SymbolHash named) {
                                     return from.implementation(named, path_id);
                                 })));
}

Features::RawResult Features::call_hierarchy_prepare(Ticket ticket,
                                                     Fid path_id,
                                                     const protocol::Position& position) {
    if(ticket.session) {
        if(auto stop = co_await nav_gate(ticket)) {
            co_return co_await stop_reply(std::move(*stop));
        }
    }
    auto cursor = cursor_at(path_id, position);
    if(!cursor)
        co_return serde_raw{"null"};
    // The item stands for the symbol, not the cursor: anchored at the
    // symbol's canonical site, expanding from a use renders the same root
    // as expanding from the declaration. A symbol no source places (an
    // implicitly declared `operator delete`) has no item.
    std::vector<protocol::CallHierarchyItem> items;
    for(auto& located: query.resolve_at(*cursor)) {
        auto kind = located.symbol.kind;
        if(kind == SymbolKind::Function || kind == SymbolKind::Method ||
           kind == SymbolKind::Operator) {
            items.push_back(
                to_lsp::call_hierarchy_item(located.symbol, located.site, located.extent));
        }
    }
    if(items.empty())
        co_return serde_raw{"null"};
    co_return to_raw(items);
}

/// The symbol a hierarchy item stands for: its handle when intact, else
/// the symbol at the item's recorded range in the file's serving source.
static std::optional<index::SymbolHash>
    item_symbol(const index::IndexQuery& query,
                Fid path_id,
                std::optional<index::IndexQuery::Cursor> at_range,
                const std::optional<protocol::LSPAny>& data) {
    if(auto hash = to_lsp::hierarchy_symbol(data); hash && query.symbol_info(*hash, path_id)) {
        return hash;
    }
    if(at_range) {
        return at_range->symbols.front();
    }
    return std::nullopt;
}

Features::RawResult Features::call_hierarchy_incoming(Fid path_id,
                                                      const protocol::CallHierarchyItem& item) {
    auto symbol =
        item_symbol(query, path_id, cursor_at(path_id, item.selection_range.start), item.data);
    if(!symbol)
        co_await kota::fail(item_not_resolved("call hierarchy"));

    std::vector<index::IndexQuery::Edge> callers;
    auto asked = sources(*symbol, path_id);
    for(auto& [from, named]: asked) {
        merge_edges(callers,
                    from->call_graph(named, path_id, {.callees = false}).callers,
                    [&](Fid file) { return answers_for(*from, file, asked); });
    }
    std::vector<protocol::CallHierarchyIncomingCall> results;
    for(auto& edge: callers) {
        results.push_back(
            {to_lsp::call_hierarchy_item(edge.symbol.symbol, edge.symbol.site, edge.symbol.extent),
             to_lsp::ranges(edge.sites)});
    }
    co_return to_raw(results);
}

Features::RawResult Features::call_hierarchy_outgoing(Fid path_id,
                                                      const protocol::CallHierarchyItem& item) {
    auto symbol =
        item_symbol(query, path_id, cursor_at(path_id, item.selection_range.start), item.data);
    if(!symbol)
        co_await kota::fail(item_not_resolved("call hierarchy"));

    std::vector<index::IndexQuery::Edge> callees;
    auto asked = sources(*symbol, path_id);
    for(auto& [from, named]: asked) {
        merge_edges(callees,
                    from->call_graph(named, path_id, {.callers = false}).callees,
                    [&](Fid file) { return answers_for(*from, file, asked); });
    }
    std::vector<protocol::CallHierarchyOutgoingCall> results;
    for(auto& edge: callees) {
        results.push_back(
            {to_lsp::call_hierarchy_item(edge.symbol.symbol, edge.symbol.site, edge.symbol.extent),
             to_lsp::ranges(edge.sites)});
    }
    co_return to_raw(results);
}

Features::RawResult Features::type_hierarchy_prepare(Ticket ticket,
                                                     Fid path_id,
                                                     const protocol::Position& position) {
    if(ticket.session) {
        if(auto stop = co_await nav_gate(ticket)) {
            co_return co_await stop_reply(std::move(*stop));
        }
    }
    auto cursor = cursor_at(path_id, position);
    if(!cursor)
        co_return serde_raw{"null"};
    // Anchored at the canonical site, like the call hierarchy item.
    std::vector<protocol::TypeHierarchyItem> items;
    for(auto& located: query.resolve_at(*cursor)) {
        auto kind = located.symbol.kind;
        if(kind == SymbolKind::Class || kind == SymbolKind::Struct || kind == SymbolKind::Enum ||
           kind == SymbolKind::Union) {
            items.push_back(
                to_lsp::type_hierarchy_item(located.symbol, located.site, located.extent));
        }
    }
    if(items.empty())
        co_return serde_raw{"null"};
    co_return to_raw(items);
}

static std::vector<protocol::TypeHierarchyItem>
    type_items(llvm::ArrayRef<index::IndexQuery::Located> types) {
    std::vector<protocol::TypeHierarchyItem> results;
    for(auto& located: types) {
        results.push_back(
            to_lsp::type_hierarchy_item(located.symbol, located.site, located.extent));
    }
    return results;
}

Features::RawResult Features::type_hierarchy_supertypes(Fid path_id,
                                                        const protocol::TypeHierarchyItem& item) {
    auto symbol =
        item_symbol(query, path_id, cursor_at(path_id, item.selection_range.start), item.data);
    if(!symbol)
        co_await kota::fail(item_not_resolved("type hierarchy"));
    std::vector<index::IndexQuery::Located> supertypes;
    auto asked = sources(*symbol, path_id);
    for(auto& [from, named]: asked) {
        merge_located(supertypes,
                      from->type_hierarchy(named, path_id, {.subtypes = false}).supertypes,
                      [&](Fid file) { return answers_for(*from, file, asked); });
    }
    co_return to_raw(type_items(supertypes));
}

Features::RawResult Features::type_hierarchy_subtypes(Fid path_id,
                                                      const protocol::TypeHierarchyItem& item) {
    auto symbol =
        item_symbol(query, path_id, cursor_at(path_id, item.selection_range.start), item.data);
    if(!symbol)
        co_await kota::fail(item_not_resolved("type hierarchy"));
    std::vector<index::IndexQuery::Located> subtypes;
    auto asked = sources(*symbol, path_id);
    for(auto& [from, named]: asked) {
        merge_located(subtypes,
                      from->type_hierarchy(named, path_id, {.supertypes = false}).subtypes,
                      [&](Fid file) { return answers_for(*from, file, asked); });
    }
    co_return to_raw(type_items(subtypes));
}

std::vector<protocol::SymbolInformation> Features::workspace_symbol(llvm::StringRef text) {
    std::vector<protocol::SymbolInformation> results;
    auto parsed = index::SymbolQuery::parse(text);
    if(!parsed) {
        return results;
    }
    // Some clients (VS Code) filter the replies against the query text
    // again, where a bare name would fail a qualified query: those
    // replies carry the qualified name.
    bool qualified = parsed->absolute || !parsed->scope.empty();
    for(auto& located: query.search(*parsed, workspace_symbol_limit)) {
        // Every project answers workspace/symbol (MasterServer): an open
        // file only from the project serving its buffer.
        if(auto* serving = open_in(located.site.file); serving && serving != &query) {
            continue;
        }
        auto container = query.container_name(located.symbol.hash);
        auto info = to_lsp::symbol_information(located.symbol, located.site, container);
        if(qualified && !container.empty()) {
            info.name = container + "::" + info.name;
        }
        results.push_back(std::move(info));
    }
    return results;
}

}  // namespace clice

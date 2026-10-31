module clice;

import :index.peer_queries;
import :server.master;
import :server.served_project;
import :support.logging;
import :support.timer;
import :worker.protocol;

namespace clice {

ipc::Error content_modified() {
    return ipc::Error{static_cast<protocol::integer>(protocol::LSPErrorCodes::ContentModified),
                      "Document changed while the request was in flight"};
}

ServingMode ServedProject::serving_of(const Session& session) const {
    auto* document = find(session.path_id);
    assert(document && document->session.get() == &session &&
           "a fresh ticket's document belongs to the project it was taken in");
    return document->serving;
}

bool ServedProject::ast_answerable(const Session& session) const {
    return index_current(session.path_id) && !compile_stopped(session);
}

kota::task<Route> ServedProject::pick_route(const Ticket& ticket,
                                            RouteOptions options,
                                            std::optional<index::IndexSource>* source) {
    for(bool waited = false;;) {
        if(!ticket.fresh()) {
            co_return Route::Outdated;
        }
        auto& session = *ticket.session;
        if(ast_answerable(session)) {
            co_return Route::Ast;
        }
        auto serving = serving_of(session);
        // An oversized buffer is not worth a synchronous main-thread lex;
        // the features computed from the index by lexing the whole buffer
        // follow the investment policy instead. Answers that only look the
        // index up serve at any size.
        bool capped = options.full_lex && session.text().size() > full_lex_cap;
        if(!capped) {
            // The document's own index when current (the answer while a
            // crash stops the compile — the worker cannot be asked, the
            // index is still true), else the disk shard admitted by
            // freshness clause 4.
            auto admitted = project.index_query.serving(session.path_id);
            // The disk shard merges every context the file compiles in, or
            // holds its first command's: a context the user chose waits for
            // its own compile.
            if(admitted && admitted->kind == index::IndexSource::Kind::Shard &&
               serving == ServingMode::Escalated && active_choice(session.path_id)) {
                admitted.reset();
            }
            if(admitted) {
                // An index answer must not strand an escalated document's
                // pending build: this request still pulls the compile it
                // would otherwise have waited on, just without blocking.
                if(serving == ServingMode::Escalated && !current(session.path_id)) {
                    request_compile(ticket.session);
                }
                if(source) {
                    *source = admitted;
                }
                co_return Route::Index;
            }
        }
        if(serving == ServingMode::Escalated) {
            co_return Route::Ast;
        }
        if(options.await_cold_attempt && !waited) {
            waited = true;
            co_await sched.pump.await_attempt(session.path_id);
            continue;
        }
        co_return Route::Empty;
    }
}

kota::task<std::optional<Stop>> ServedProject::navigation_gate(const Ticket& ticket) {
    switch(co_await pick_route(ticket, {})) {
        case Route::Outdated: co_return Stop{content_modified()};
        // The index sources resolve the cursor under clauses 1/4 — or
        // reject it, which the query layer answers as empty. Either way
        // no compile is owed.
        case Route::Index:
        case Route::Empty: co_return std::nullopt;
        case Route::Ast: break;
    }
    // Same posture as every AST-backed request: the document's file index
    // is produced by the very compile awaited here, so once this finishes
    // the index describes the buffer.
    co_return co_await await_compile(ticket);
}

kota::task<std::optional<Stop>> ServedProject::await_compile(const Ticket& ticket) {
    // A failed compile yields null rather than a lookup against a buffer
    // with no finished index; a superseded one tells the client to re-pull.
    bool compiled = co_await ensure_compiled(ticket.session);
    if(!ticket.fresh()) {
        co_return Stop{content_modified()};
    }
    if(!compiled) {
        co_return Stop{};
    }
    co_return std::nullopt;
}

std::optional<index::IndexQuery::Cursor>
    ServedProject::cursor_at(Fid path_id, const protocol::Position& position) const {
    // A document that names no file (an `untitled:` buffer) has no place
    // in the index.
    if(!path_id.valid()) {
        return std::nullopt;
    }
    return project.index_query.symbol_at(path_id, position, server.position_encoding);
}

index::PeerQueries ServedProject::peer_queries() {
    return {
        .own = project.index_query,
        .peers = [this] { return server.peer_queries(*this); },
        .open_in = [this](Fid file) { return server.owner_query(file); },
    };
}

SourceText& ServedProject::text_of(Session& session) {
    session.source->set_dialect(dialect(session.path_id));
    return *session.source;
}

SearchConfig ServedProject::search_of(Fid path_id) {
    // Editor use: the search follows the command (context choice, chosen
    // database entry) the open buffer compiles under.
    std::string directory;
    std::vector<std::string> arguments;
    return project.toolchain.search_config(resolve_command(path_id, directory, arguments).ref);
}

std::vector<index::DocumentLink> ServedProject::preamble_links(const Session& session) const {
    auto state = project.index_query.preamble_index(session.path_id);
    return state ? state->links() : std::vector<index::DocumentLink>{};
}

kota::task<std::vector<index::DocumentLink>, ipc::Error>
    ServedProject::directive_links(const Ticket& ticket, kota::cancellation_token token) {
    auto result = co_await ask_worker(ticket,
                                      feature::RequestKind::DocumentLink,
                                      "DocumentLink",
                                      worker::DocumentLinkParams{},
                                      std::move(token))
                      .or_fail();
    // The preamble is compiled into the PCH, so the worker's AST only
    // covers the rest of the file — merge the preamble's links in front.
    auto links = preamble_links(*ticket.session);
    links.insert(links.end(), result.begin(), result.end());
    co_return links;
}

bool ServedProject::admits(const Session& session,
                           feature::RequestKind kind,
                           const Session::Attempt* attempt,
                           bool parses) const {
    auto now = Session::Clock::now();
    if(attempt ? attempt->crashed_meanwhile() : session.stops(kind, now)) {
        return false;
    }
    return !parses || !compile_stopped(session);
}

kota::task<std::optional<worker::FormatResult>, ipc::Error>
    ServedProject::format(const Ticket& ticket,
                          std::optional<protocol::Range> range,
                          kota::cancellation_token token) {
    auto& session = *ticket.session;
    auto& files = project.file_table;
    auto path = std::string(files.resolve(session.path_id));
    auto kind = feature::RequestKind::Format;

    if(!admits(session, kind, nullptr, false)) {
        LOG_DEBUG("Format: {} is stopped by a crash", path);
        co_return std::nullopt;
    }
    Session::Attempt attempt(session, kind);

    worker::FormatParams params;
    params.file = files.display(session.path_id);
    params.text = session.text().str();
    if(range) {
        params.range = session.positions().offset_range(*range, server.position_encoding);
    }

    ScopedTimer timer;
    bool refused = false;
    auto result = co_await send(ticket, kind, params, attempt, false, token, refused);
    if(refused) {
        LOG_DEBUG("Format: {} was stopped by a crash before it reached a worker", path);
        if(!ticket.fresh()) {
            co_await kota::fail(content_modified());
        }
        co_return std::nullopt;
    }
    result = finish(ticket, kind, "Format", std::move(result));
    if(!result.has_value()) {
        co_return kota::outcome_error(std::move(result.error()));
    }
    LOG_PERF("request", "kind=Format file={} total_ms={:.2f}", path, timer.ms_f());
    co_return std::move(*result);
}

}  // namespace clice

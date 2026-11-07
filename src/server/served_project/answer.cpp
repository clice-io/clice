module clice;

import :feature.document_links;
import :index.project_index;
import :index.query;
import :index.shard;
import :server.master;
import :server.served_project;
import :support.logging;
import :syntax.scan;
import :vfs.file_system;
import :vfs.path;
import :support.timer;
import :worker.protocol;

namespace clice {

ipc::Error content_modified() {
    return ipc::Error{static_cast<protocol::integer>(protocol::LSPErrorCodes::ContentModified),
                      "Document changed while the request was in flight"};
}

bool ServedProject::compiles(const Session& session) const {
    assert(owns(session) && "a fresh ticket's document belongs to the project it was taken in");
    return find_document(session.path_id)->compiles;
}

async::task<Route> ServedProject::pick_route(const Ticket& ticket,
                                             RouteOptions options,
                                             std::optional<index::IndexSource>* source) {
    for(bool waited = false;;) {
        if(!ticket.fresh()) {
            co_return Route::Outdated;
        }
        auto& session = *ticket.session;
        // A current AST answers, unless a crash stops the compile: then the
        // index answers before a compile is awaited, which a stopped
        // document's request would find failed.
        if(index_current(session.path_id) && !compile_stopped(session)) {
            co_return Route::AST;
        }
        bool compiled = compiles(session);
        // An oversized buffer is not worth a synchronous main-thread lex;
        // the features computed from the index by lexing the whole buffer
        // wait for the compile instead. Answers that only look the index up
        // serve at any size.
        bool capped = options.lexes_whole_buffer && session.text().size() > max_lexed_buffer;
        if(!options.needs_ast && !capped) {
            // The document's own index when current (the answer while a
            // crash stops the compile — the worker cannot be asked, the
            // index is still true), else the disk shard admitted by
            // freshness clause 4.
            auto admitted = query().serving(session.path_id);
            // The disk shard merges every context the file compiles in, or
            // holds its first command's: a context the user chose, or one
            // the user wants alone, waits for its own compile.
            if(admitted && admitted->kind == index::IndexSource::Kind::Shard && compiled &&
               (active_choice(session.path_id) ||
                server.own_context_only.contains(session.path_id))) {
                admitted.reset();
            }
            if(admitted) {
                // An index answer must not strand a compiled document's
                // pending build: this request still pulls the compile it
                // would otherwise have waited on, just without blocking.
                if(compiled && !current(session.path_id)) {
                    request_compile(ticket.session);
                }
                if(source) {
                    *source = admitted;
                }
                co_return Route::Index;
            }
        }
        if(compiled) {
            co_return Route::AST;
        }
        if(options.waits_for_first_index && !waited) {
            waited = true;
            co_await project.background_index.await_attempt(session.path_id);
            continue;
        }
        co_return Route::Empty;
    }
}

async::task<std::optional<GateStop>> ServedProject::navigation_gate(const Ticket& ticket) {
    switch(co_await pick_route(ticket, {})) {
        case Route::Outdated: co_return GateStop{content_modified()};
        // The index sources resolve the cursor under clauses 1/4 — or
        // reject it, which the query layer answers as empty. Either way
        // no compile is owed.
        case Route::Index:
        case Route::Empty: co_return std::nullopt;
        case Route::AST: break;
    }
    // Same posture as every AST-backed request: the document's file index
    // is produced by the very compile awaited here, so once this finishes
    // the index describes the buffer. The index needs no AST: an evicted
    // document's is current already.
    co_return co_await await_compile(ticket, {.ast = false});
}

async::task<std::optional<GateStop>> ServedProject::await_compile(const Ticket& ticket,
                                                                  EnsureOptions options) {
    // A failed compile yields null rather than a lookup against a buffer
    // with no finished index; an outdated one tells the client to re-pull.
    bool compiled = co_await ensure_compiled(ticket.session, options);
    if(!ticket.fresh()) {
        co_return GateStop{content_modified()};
    }
    if(!compiled) {
        co_return GateStop{};
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
    return query().symbol_at(path_id, position, server.position_encoding);
}

/// The document's merged index (ServedDocument::merged), made if missing:
/// while its compile describes the buffer, the buffer is the text the
/// file's shard indexed, and the compile skipped some code — unless the
/// user asked for the file's own context alone. Null otherwise.
const static ServedDocument::MergedIndex* merged_index(const ServedProject& served,
                                                       Fid path_id,
                                                       const ServedDocument& document) {
    auto& compiled = document.latest;
    auto& session = *served.server.find_session(path_id);
    if(!compiled || !compiled->index || !compiled->index->loaded() ||
       compiled->inactive_regions.empty() ||
       served.server.own_context_only.contains(session.path_id)) {
        document.merged.reset();
        return nullptr;
    }
    auto& state = *compiled->index;
    auto& own = state.shard_of(state.focus_file());
    if(!served.current(session.path_id) &&
       !own.matches_content(session.text().size(), session.hash)) {
        document.merged.reset();
        return nullptr;
    }
    if(document.merged && document.merged->index == compiled->index) {
        return &*document.merged;
    }
    auto* stored = served.project.index.shard(session.path_id);
    if(!stored || !stored->matches_content(session.text().size(), session.hash)) {
        document.merged.reset();
        return nullptr;
    }
    auto merged = index::merge_contexts(*stored,
                                        served.project.index.live_variants(session.path_id),
                                        own,
                                        compiled->inactive_regions);
    if(!merged) {
        document.merged.reset();
        return nullptr;
    }
    document.merged = ServedDocument::MergedIndex{
        .index = compiled->index,
        .shard = std::make_shared<index::Shard>(std::move(merged->shard)),
        .additions = merged->additions,
    };
    return &*document.merged;
}

std::vector<index::OpenBuffer> ServedProject::open_buffers() const {
    std::vector<index::OpenBuffer> open;
    for(auto& [path_id, document]: documents) {
        auto& session = *server.find_session(path_id);
        auto& compiled = document.latest;
        auto* merged = merged_index(*this, path_id, document);
        open.push_back({
            .file = path_id,
            .text = session.text(),
            .hash = session.hash,
            .positions = session.positions(),
            .index = compiled ? compiled->index : nullptr,
            .current = current(path_id),
            .preamble = compiled ? compiled->preamble_index : nullptr,
            .merged = merged ? merged->shard : nullptr,
        });
    }
    return open;
}

ServedProject::SkippedCode ServedProject::skipped_code(Fid file) const {
    auto* document = find_document(file);
    auto* merged = document ? merged_index(*this, file, *document) : nullptr;
    if(!merged) {
        return {};
    }
    return {
        .regions = document->latest->inactive_regions,
        .indexed = merged->additions != 0,
    };
}

bool ServedProject::merge_again(Fid file) const {
    auto* document = find_document(file);
    if(!document) {
        return false;
    }
    auto before = document->merged ? document->merged->additions : 0;
    document->merged.reset();
    auto* merged = merged_index(*this, file, *document);
    return before != (merged ? merged->additions : 0);
}

index::IndexQuery ServedProject::query_all() const {
    std::vector<index::IndexQuery> others;
    for(auto& other: server.projects) {
        if(other.get() != this) {
            others.push_back(other->query());
        }
    }
    return query().asking(std::move(others));
}

index::IndexQuery ServedProject::editing_query() const {
    std::vector<index::OpenBuffer> open;
    for(auto& other: server.projects) {
        if(other.get() == this || (!project.root.empty() && !other->project.root.empty() &&
                                   path::is_within(other->project.root, project.root))) {
            open.append_range(other->open_buffers());
        }
    }
    return project.index_query.reading(std::move(open));
}

SourceText& ServedProject::text_of(Session& session) {
    session.source->set_dialect(dialect(session.path_id));
    return *session.source;
}

SearchConfig ServedProject::search_of(Fid path_id) {
    // Editor use: the search follows the command (context choice, chosen
    // database entry) the open buffer compiles under.
    return project.toolchain.search_config(
        project.select_command(path_id, command_request(path_id)).ref);
}

ScanResult ServedProject::declared_module(Fid path_id, llvm::StringRef text) {
    auto rendered = project.render(project.select_command(path_id, command_request(path_id)).ref);
    std::vector<const char*> argv;
    argv.reserve(rendered.arguments.size());
    for(auto& argument: rendered.arguments) {
        argv.push_back(argument.c_str());
    }
    return scan_module_decl(argv,
                            rendered.directory,
                            text,
                            llvm::makeIntrusiveRefCnt<vfs::View>(std::move(rendered.hidden)));
}

std::vector<index::DocumentLink> ServedProject::preamble_links(const Session& session) const {
    auto state = query().preamble_index(session.path_id);
    return state ? state->links() : std::vector<index::DocumentLink>{};
}

async::task<std::vector<index::DocumentLink>, ipc::Error>
    ServedProject::directive_links(const Ticket& ticket, async::cancellation_token token) {
    auto result = co_await ast_request(ticket,
                                       feature::RequestKind::DocumentLink,
                                       "DocumentLink",
                                       worker::DocumentLinkParams{},
                                       std::move(token))
                      .or_fail();
    // The preamble is compiled into the PCH, so the worker's AST only
    // covers the rest of the file — merge the preamble's links in front.
    auto& session = *ticket.session;
    auto links = preamble_links(session);
    links.insert(links.end(), result.begin(), result.end());
    // An include leaves no index entry: the skipped code's includes link
    // even when the index holds no entries there.
    auto skipped = skipped_code(session.path_id).regions;
    if(skipped.empty() || session.text().size() > max_lexed_buffer) {
        co_return links;
    }
    co_return feature::merge(std::move(links),
                             feature::document_links(text_of(session), query(), session.path_id),
                             skipped);
}

bool ServedProject::admits(const Session& session,
                           feature::RequestKind kind,
                           const Session::Dispatch* dispatch,
                           bool parses) const {
    auto now = Session::Clock::now();
    if(dispatch ? dispatch->crashed_meanwhile() : session.crash_stops(kind, now)) {
        return false;
    }
    return !parses || !compile_stopped(session);
}

async::task<std::optional<worker::FormatResult>, ipc::Error>
    ServedProject::format(const Ticket& ticket,
                          std::optional<protocol::Range> range,
                          async::cancellation_token token) {
    auto& session = *ticket.session;
    auto& files = project.file_table;
    auto path = std::string(files.resolve(session.path_id));
    auto kind = feature::RequestKind::Format;

    if(!admits(session, kind, nullptr, false)) {
        LOG_DEBUG("Format: {} is stopped by a crash", path);
        co_return std::nullopt;
    }
    Session::Dispatch dispatch(session, kind);

    worker::FormatParams params;
    params.file = files.display(session.path_id);
    params.text = session.text().str();
    if(range) {
        params.range = session.positions().offset_range(*range, server.position_encoding);
    }

    ScopedTimer timer;
    bool refused = false;
    auto result = co_await send_stateless(ticket, kind, params, dispatch, false, token, refused);
    if(refused) {
        LOG_DEBUG("Format: {} was stopped by a crash before it reached a worker", path);
        if(!ticket.fresh()) {
            co_await async::fail(content_modified());
        }
        co_return std::nullopt;
    }
    result = finish(ticket, kind, "Format", std::move(result));
    if(!result.has_value()) {
        co_return kotatsu::outcome_error(std::move(result.error()));
    }
    LOG_PERF("request", "kind=Format file={} total_ms={:.2f}", path, timer.ms_f());
    co_return std::move(*result);
}

}  // namespace clice

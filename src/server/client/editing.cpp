module clice;

import :command.search_config;
import :feature.code_actions;
import :feature.code_completion;
import :feature.protocol;
import :feature.rename;
import :feature.signature_help;
import :project.build;
import :project.queries;
import :server.client;
import :server.master;
import :server.served_project;
import :support.json;
import :syntax.include_resolver;
import :syntax.source_text;
import :vfs.dir_cache;
import :vfs.file_system;
import :worker.protocol;

namespace clice {

/// A request the server cannot carry out, and why.
static ipc::Error request_failed(std::string message) {
    return ipc::Error{static_cast<protocol::integer>(protocol::LSPErrorCodes::RequestFailed),
                      std::move(message)};
}

/// Fold versioned document changes into the plain `changes` map for a
/// client without documentChanges support.
static void unversion(protocol::WorkspaceEdit& edit) {
    if(!edit.document_changes) {
        return;
    }
    std::map<protocol::DocumentUri, std::vector<protocol::TextEdit>> changes;
    for(auto& change: *edit.document_changes) {
        auto* document = std::get_if<protocol::TextDocumentEdit>(&change);
        if(!document) {
            continue;
        }
        auto& edits = changes[document->text_document.uri];
        for(auto& text_edit: document->edits) {
            if(auto* plain = std::get_if<protocol::TextEdit>(&text_edit)) {
                edits.push_back(std::move(*plain));
            }
        }
    }
    edit.changes = std::move(changes);
    edit.document_changes.reset();
}

/// The project's query reading every open document as the project serving
/// it does: another project's open file answers from its buffer's index,
/// not from the disk shard this project holds of it.
static index::IndexQuery open_documents_query(ServedProject& project) {
    auto query = project.project.index_query;
    query.attach(project.server);
    return query;
}

void LSPClient::register_editing() {
    // Code completion. Serves preamble contexts (include/import) locally
    // from the include graph and module map; asks a stateless worker for
    // ordinary code completion. Pauses background indexing for the
    // request's span. Space-triggered requests are only answered for import
    // contexts.
    on_document<protocol::CompletionParams>(
        [this](RequestContext& ctx, const auto& params, ResolvedDoc document) -> RawResult {
            auto& [path, path_id, session, served, version, dispatched] = document;
            auto& project = *served;
            // The task starts once the messages read with the request are
            // dispatched: a $/cancelRequest among them (rapid-fire completions
            // cancel and re-issue as the user types) keeps it from starting, and
            // an edit arrives before the offset and completion context are
            // computed, so the synchronous include scan below never serves
            // candidates or ranges for a buffer that no longer exists. Unlike the
            // whole-document features, such an edit is served, not refused: the
            // ticket is taken here, and the client filters candidates against
            // whatever it typed meanwhile.
            //
            // Those messages may also have closed the document, replaced it with
            // a reopen, or moved it to another project: the request belongs to
            // the buffer this project served.
            if(!project.owns(*session)) {
                co_await kota::fail(content_modified());
            }
            auto ticket = Ticket::take(session);
            // Asking for code completion is edit intent: the document is
            // compiled from now on (a no-op when it already is or under
            // readonly = "on").
            project.escalate(path_id);
            auto pause = project.sched.pump.scoped_pause();

            llvm::StringRef trigger;
            if(params.context && params.context->trigger_character) {
                trigger = *params.context->trigger_character;
            }
            auto& settings = project.project.settings;
            auto& query = project.project.index_query;
            auto& files = project.project.file_table;
            auto encoding = server.position_encoding;
            auto resolved = std::string(files.resolve(path_id));
            auto positions = session->positions();
            vfs::Scope scope(files.dirs);
            auto start = feature::start_completion({
                .file = path_id,
                .path = resolved,
                .text = session->text(),
                .positions = positions,
                .position = params.position,
                .encoding = encoding,
                .trigger_character = trigger,
                .dialect = project.dialect(path_id),
                .options = settings.code_completion,
                .client = completion_client,
                .query = query,
                .graph = project.project.dep_graph,
                .files = files,
                .search = [&] { return project.search_of(path_id); },
                .scope = scope,
            });
            if(std::holds_alternative<feature::NoCompletion>(start)) {
                co_return codec::RawValue{"[]"};
            }
            if(auto* list = std::get_if<protocol::CompletionList>(&start)) {
                co_return to_raw(*list);
            }
            auto& compile = std::get<feature::CompileCompletion>(start);
            auto result = co_await project
                              .interactive(ticket,
                                           feature::RequestKind::Completion,
                                           "Completion",
                                           positions.offset_clamped(params.position, encoding),
                                           worker::CompletionParams{
                                               .client = completion_client,
                                               .encoding = encoding,
                                               .candidates = compile.candidates,
                                               .more_candidates = compile.more_candidates,
                                           },
                                           ctx.cancellation)
                              .or_fail();
            if(!result.include) {
                co_return std::move(result.list);
            }

            protocol::CompletionList list;
            [[maybe_unused]] auto decoded =
                json::from_string<ipc::lsp_config>(result.list.data, list);
            assert(decoded && "the worker encodes the list");
            co_return to_raw(feature::finish_completion(std::move(list),
                                                        *result.include,
                                                        compile.candidates,
                                                        query,
                                                        path_id,
                                                        project.search_of(path_id),
                                                        scope,
                                                        settings.code_completion.limit));
        });

    // Signature help, asked of a stateless worker. Pauses background
    // indexing for the request's span.
    on_document<protocol::SignatureHelpParams>(
        [this](RequestContext& ctx, const auto& params, ResolvedDoc document) -> RawResult {
            auto& [path, path_id, session, served, version, ticket] = document;
            auto& project = *served;
            if(auto offset = session->positions().offset(params.position, server.position_encoding);
               offset &&
               !feature::can_hold_call(session->text(), *offset, project.dialect(path_id))) {
                co_return codec::RawValue{"null"};
            }
            project.escalate(path_id);
            auto pause = project.sched.pump.scoped_pause();
            // The popup the client shows: the user may have moved it to another
            // overload.
            std::string selected;
            if(params.context && params.context->active_signature_help) {
                auto& shown = *params.context->active_signature_help;
                auto active = shown.active_signature.value_or(0);
                if(active < shown.signatures.size()) {
                    selected = shown.signatures[active].label;
                }
            }
            co_return co_await project.interactive(
                ticket,
                feature::RequestKind::SignatureHelp,
                "SignatureHelp",
                session->positions().offset_clamped(params.position, server.position_encoding),
                worker::SignatureHelpParams{.selected = selected},
                ctx.cancellation);
        });

    // Whole-document formatting, or a range's, on a stateless worker. Pauses
    // background indexing for the request's span.
    auto format = [this](ResolvedDoc document,
                         std::optional<protocol::Range> range,
                         kota::cancellation_token token) -> RawResult {
        auto& project = *document.project;
        auto pause = project.sched.pump.scoped_pause();
        auto result = co_await project.format(document.ticket, range, token).or_fail();
        if(!result) {
            co_return codec::RawValue{"null"};
        }
        if(!result->error.empty()) {
            co_await kota::fail(request_failed(std::move(result->error)));
        }
        auto edits = feature::to_protocol(result->edits,
                                          document.session->positions(),
                                          server.position_encoding);
        assert(edits && "the edits are of the text the ticket was taken on");
        co_return to_raw(*edits);
    };
    on_document<protocol::DocumentFormattingParams>(
        [format](RequestContext& ctx, const auto& params, ResolvedDoc document) {
            return format(std::move(document), std::nullopt, ctx.cancellation);
        });
    on_document<protocol::DocumentRangeFormattingParams>(
        [format](RequestContext& ctx, const auto& params, ResolvedDoc document) {
            return format(std::move(document), params.range, ctx.cancellation);
        });

    // Code actions on a range of the buffer: the worker computes them
    // against its AST, and their index requests are resolved against the
    // open documents as their projects serve them (feature::resolve_actions).
    // The edits name the document the request came from, a second name
    // sharing the first name's buffer included.
    on_document<protocol::CodeActionParams>([this](RequestContext& ctx,
                                                   const auto& params,
                                                   ResolvedDoc document) -> RawResult {
        auto& [path, path_id, session, served, version, ticket] = document;
        auto& project = *served;
        auto encoding = server.position_encoding;
        auto& files = project.project.file_table;
        // The client's kind filter (LSP CodeActionContext.only); empty keeps
        // all.
        auto only = params.context.only.value_or(std::vector<protocol::CodeActionKind>());
        // Only a user asking for actions gets the switches, which look the
        // listings up: an editor asks on every move of the cursor.
        bool offer_switches =
            params.context.trigger_kind != protocol::CodeActionTriggerKind::Automatic;

        // Code actions are AST products the index cannot compute; a document
        // the policy keeps un-compiled answers honestly empty rather than
        // forcing the compile the policy declined.
        std::vector<protocol::CodeAction> result;
        if(feature::admits_any(only) && (project.ast_answerable(*session) ||
                                         project.serving_of(*session) != ServingMode::IndexOnly)) {
            // Clamped against the buffer the ticket was taken on: a buffer
            // that moves before the reply arrives turns the reply into
            // ContentModified.
            auto actions =
                co_await project
                    .ask_worker(
                        ticket,
                        feature::RequestKind::CodeAction,
                        "CodeAction",
                        worker::CodeActionParams{
                            .range = session->positions().offset_range(params.range, encoding),
                        },
                        ctx.cancellation)
                    .or_fail();

            auto shown = files.display(path_id);
            Fid host = project.host_of(path_id);
            auto host_session = host.valid() ? server.find_session(host) : nullptr;
            auto host_path = host.valid() ? files.display(host) : std::string();
            // A closed host formats as the build spells it.
            auto host_style_path =
                host_session || !host.valid() ? host_path : files.spelling(host).str();
            std::optional<SourceText> host_disk;
            std::optional<SearchConfig> search;
            auto query = open_documents_query(project);
            vfs::Scope scope(files.dirs);
            auto resolved = feature::resolve_actions(
                std::move(actions),
                only,
                {
                    .query = &query,
                    .file = path_id,
                    .path = shown,
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
                                search = project.search_of(path_id);
                            }
                            return include_argument(header,
                                                    *search,
                                                    files.spelling(path_id),
                                                    scope);
                        },
                });
            result = feature::to_protocol(
                std::move(resolved),
                session->positions(),
                encoding,
                {.uri = feature::to_uri(path), .version = version},
                {.uri = host.valid() ? feature::to_uri(host_path) : std::string(),
                 .version = host_session ? std::optional(host_session->version) : std::nullopt});
            if(!versioned_edits) {
                for(auto& action: result) {
                    unversion(*action.edit);
                }
            }
        }
        // Listed after the compile the actions waited for: a header's first
        // one decides whether it is self-contained, which changes its listing
        // and outdates the epoch before.
        if(!offer_switches || !project.find(path_id)) {
            co_return to_raw(result);
        }
        auto uri = feature::to_uri(files.display(path_id));
        if(feature::admits(only, context_action_kind)) {
            auto current = co_await server.current_context(path_id);
            auto listing = server.query_contexts(path_id, {.uri = uri});
            result.append_range(context_actions(uri, current, listing));
        }
        if(feature::admits(only, configuration_action_kind)) {
            result.append_range(configuration_actions(uri, list_configurations(project.project)));
        }
        co_return to_raw(result);
    });

    // Rename from the project's index (see feature/rename.cppm), never a
    // recompile. prepareRename answers the name token at the position, or an
    // error saying why the symbol there cannot be renamed.
    on_document<protocol::PrepareRenameParams>(
        [this](RequestContext& ctx, const auto& params, ResolvedDoc document) -> RawResult {
            auto& project = *document.project;
            // Refused up front, before the cursor costs a compile.
            if(auto refused = feature::rename_refusal(project.project.root)) {
                co_await kota::fail(request_failed(std::move(*refused)));
            }
            co_return co_await navigate(
                project,
                document.ticket,
                document.path_id,
                params.position,
                "null",
                [&](const index::IndexQuery::Cursor& cursor) -> Reply {
                    auto renamed = feature::rename_at(project.project.index_query, cursor);
                    if(!renamed) {
                        return std::unexpected(request_failed(std::move(renamed.error())));
                    }
                    return to_raw(protocol::PrepareRenamePlaceholder{
                        .range = feature::to_range(renamed->token, server.position_encoding),
                        .placeholder = renamed->target.symbol.symbol.name,
                    });
                });
        },
        {.closed = true});

    // Null when nothing at the position can be renamed or the file fails to
    // compile; an error names the conflicts, or the files whose index is not
    // current, that keep the rename from being made. The open documents are
    // read as the editor holds them, and as the projects they belong to
    // index them.
    on_document<protocol::RenameParams>(
        [this](RequestContext& ctx, const auto& params, ResolvedDoc document) -> RawResult {
            auto& [path, path_id, session, served, version, ticket] = document;
            auto& project = served->project;
            auto& position = params.position;
            auto& new_name = params.new_name;
            // Refused up front, before the cursor costs a compile.
            if(auto refused = feature::rename_refusal(project.root)) {
                co_await kota::fail(request_failed(std::move(*refused)));
            }
            if(session) {
                if(auto stop = co_await served->navigation_gate(ticket)) {
                    co_return co_await stop_reply(std::move(*stop));
                }
            }
            auto query = open_documents_query(*served);
            auto cursor = served->cursor_at(path_id, position);
            if(!cursor) {
                co_return codec::RawValue{"null"};
            }
            auto at = feature::rename_at(query, *cursor);
            if(!at) {
                co_await kota::fail(request_failed(std::move(at.error())));
            }

            // The walk, the reads and the scan run off the loop; only the
            // files spelling a name come back with their text.
            EditableFiles editable_files(project.root, project.settings);
            auto texts = co_await kota::queue(
                [editable_files,
                 names = llvm::StringSet<>{at->target.symbol.symbol.name, new_name}] {
                    return feature::RenameTexts(editable_files.sources(), names);
                });
            if(session && !ticket.fresh()) {
                co_await kota::fail(content_modified());
            }
            // Entries indexed during the scan may link the symbol to more:
            // plan with the group they give now.
            cursor = served->cursor_at(path_id, position);
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

            auto editable = [&](llvm::StringRef file) {
                return editable_files.editable(file);
            };
            // Open buffers are read as the editor holds them, whichever
            // project serves them.
            auto buffer = [&](llvm::StringRef file) -> std::optional<llvm::StringRef> {
                auto id = project.file_table.find(Spelling::absolute(file));
                auto open = id ? server.find_session(*id) : nullptr;
                return open ? std::optional(open->text()) : std::nullopt;
            };
            auto read = [&](llvm::StringRef file) {
                return texts.read(file, buffer);
            };
            auto plan = feature::plan_rename(query,
                                             project.file_table,
                                             at->target,
                                             new_name,
                                             {.files = texts.files,
                                              .editable = editable,
                                              .read = read,
                                              .units_pending = units_pending(project)});
            if(plan.blocked()) {
                co_await kota::fail(request_failed(feature::refusal(plan)));
            }
            auto renamed = feature::to_protocol(
                plan,
                new_name,
                [&](Fid file) -> std::optional<int> {
                    auto open = server.find_session(file);
                    return open ? std::optional(open->version) : std::nullopt;
                },
                server.position_encoding);
            auto& [edit, notice] = renamed;
            if(!versioned_edits) {
                unversion(edit);
            }
            if(!notice.empty()) {
                peer.send_notification(protocol::ShowMessageParams{
                    .type = protocol::MessageType::Warning,
                    .message = std::move(notice),
                });
            }
            co_return to_raw(edit);
        },
        {.closed = true});
}

}  // namespace clice

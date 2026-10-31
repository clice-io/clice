module clice;

import :feature.document_links;
import :feature.hierarchy;
import :feature.navigation;
import :feature.protocol;
import :feature.workspace_symbols;
import :server.client;
import :server.master;
import :server.served_project;
import :support.json;

namespace clice {

using Cursor = index::IndexQuery::Cursor;

/// Error response when a call/type hierarchy item cannot be resolved back to
/// an indexed symbol.
static ipc::Error item_not_resolved(llvm::StringRef kind) {
    return ipc::Error{protocol::ErrorCode::InvalidParams,
                      std::format("Failed to resolve {} item", kind)};
}

/// The indexed symbol a hierarchy item names, or the error saying it names
/// none.
template <typename Item>
static std::expected<index::SymbolHash, ipc::Error>
    item_symbol(const ServedProject& project, Fid path_id, const Item& item, llvm::StringRef kind) {
    auto symbol = feature::item_symbol(project.project.index_query,
                                       path_id,
                                       project.cursor_at(path_id, item.selection_range.start),
                                       item.data);
    if(!symbol) {
        return std::unexpected(item_not_resolved(kind));
    }
    return *symbol;
}

void LSPClient::register_navigation() {
    // The index answers navigation for a document that is not open, whose
    // ticket has no session: an empty result is a real answer, never an
    // error.

    // Go-to-definition, assembled across all providers: preamble directive
    // targets, the index, and the directives the worker's AST sees, with an
    // index retry after the compile refreshes a dirty document.
    on_document<protocol::DefinitionParams>(
        [this](RequestContext& ctx, const auto& params, ResolvedDoc document) -> RawResult {
            auto& [path, path_id, session, served, version, ticket] = document;
            auto& project = *served;
            auto& position = params.position;
            auto encoding = server.position_encoding;
            auto& query = project.project.index_query;
            auto& files = project.project.file_table;
            if(session) {
                if(auto stop = co_await project.navigation_gate(ticket)) {
                    co_return co_await stop_reply(std::move(*stop));
                }
            }

            // Preamble include lines first: they have no symbol occurrence
            // in the index and are invisible to the worker's AST. A latest
            // compile still not current after the awaited one means the
            // world was re-dirtied mid-flight (the round committed as
            // bounded staleness): the cached links may describe a pre-edit
            // preamble — skip, and let the index and worker paths below
            // answer.
            auto offset = session ? session->positions().offset(position, encoding) : std::nullopt;
            if(offset && project.current(path_id)) {
                auto links = project.preamble_links(*session);
                if(auto* link = feature::link_at(links, *offset)) {
                    co_return to_raw(std::vector{feature::directive_location(files, *link)});
                }
            }

            // The eager index query is safe for dirty documents too:
            // freshness clause 4 serves their shard only while the buffer is
            // byte-identical, and rejects the mixed-view lookup otherwise.
            auto index_definition = [&]() -> std::vector<protocol::Location> {
                auto cursor = project.cursor_at(path_id, position);
                if(!cursor) {
                    return {};
                }
                return feature::to_locations(feature::definition(project.peer_queries(), *cursor),
                                             encoding);
            };
            if(auto result = index_definition(); !result.empty()) {
                co_return to_raw(result);
            }

            // A closed file has no worker leg: the index's answer is the
            // answer.
            if(!session) {
                co_return codec::RawValue{"[]"};
            }

            // An index-only document never owes the compile the worker query
            // implies, a document served under freshness clause 4
            // (escalated, compile still in flight) already routed to the
            // index, and a document whose compile a crash stops cannot reach
            // a worker at all. What the worker leg covers — include
            // directives, which have no symbol occurrence — the manifest
            // edges answer instead, under the same content gate as the
            // index's links: manifest lines are meaningless against a buffer
            // the index never described.
            auto serving = query.serving(path_id);
            bool shard_served = serving && serving->kind == index::IndexSource::Kind::Shard;
            if(project.serving_of(*session) == ServingMode::IndexOnly ||
               project.compile_stopped(*session) ||
               (shard_served && !project.cursor_at(path_id, position))) {
                if(offset) {
                    auto links = feature::document_links(project.text_of(*session), query, path_id);
                    if(auto* link = feature::link_at(links, *offset)) {
                        co_return to_raw(std::vector{feature::directive_location(files, *link)});
                    }
                }
                co_return codec::RawValue{"[]"};
            }

            // A symbol the shard names and no index places waits for the
            // compile: a module's declaration before the module's unit is
            // indexed is in the compile's own index alone. Awaited here, as
            // the links query skips the compile while a crash stops link
            // queries.
            if(shard_served) {
                if(auto stop = co_await project.await_compile(ticket)) {
                    co_return co_await stop_reply(std::move(*stop));
                }
            }

            // A worker error is final: a ContentModified in particular must
            // not be replaced by an index answer computed on the newer
            // buffer.
            auto links = co_await project.directive_links(ticket, ctx.cancellation).or_fail();
            if(auto* link = offset ? feature::link_at(links, *offset) : nullptr) {
                co_return to_raw(std::vector{feature::directive_location(files, *link)});
            }

            // The buffer compiled since the first lookup: retry against the
            // refreshed compile, but only when the compile actually
            // completed — a failed compile leaves the latest one non-current
            // and the caches stale.
            if(project.current(path_id)) {
                if(auto retry = index_definition(); !retry.empty()) {
                    co_return to_raw(retry);
                }
            }
            co_return codec::RawValue{"[]"};
        },
        {.closed = true});

    on_document<protocol::ReferenceParams>(
        [this](RequestContext& ctx, const auto& params, ResolvedDoc document) -> RawResult {
            auto& project = *document.project;
            co_return co_await navigate(
                project,
                document.ticket,
                document.path_id,
                params.position,
                "[]",
                [&](const Cursor& cursor) -> Reply {
                    auto limit = project.project.settings.references.limit.value;
                    return to_raw(feature::to_locations(
                        feature::references(project.peer_queries(),
                                            cursor,
                                            params.context.include_declaration,
                                            limit),
                        server.position_encoding));
                });
        },
        {.closed = true});

    on_document<protocol::DocumentHighlightParams>(
        [this](RequestContext& ctx, const auto& params, ResolvedDoc document) -> RawResult {
            auto& project = *document.project;
            co_return co_await navigate(
                project,
                document.ticket,
                document.path_id,
                params.position,
                "[]",
                [&](const Cursor& cursor) -> Reply {
                    return to_raw(feature::to_protocol(
                        feature::document_highlights(project.project.index_query, cursor),
                        server.position_encoding));
                });
        },
        {.closed = true});

    on_document<protocol::DeclarationParams>(
        [this](RequestContext& ctx, const auto& params, ResolvedDoc document) -> RawResult {
            auto& project = *document.project;
            co_return co_await navigate(
                project,
                document.ticket,
                document.path_id,
                params.position,
                "[]",
                [&](const Cursor& cursor) -> Reply {
                    return to_raw(
                        feature::to_locations(feature::declaration(project.peer_queries(), cursor),
                                              server.position_encoding));
                });
        },
        {.closed = true});

    on_document<protocol::TypeDefinitionParams>(
        [this](RequestContext& ctx, const auto& params, ResolvedDoc document) -> RawResult {
            auto& project = *document.project;
            co_return co_await navigate(
                project,
                document.ticket,
                document.path_id,
                params.position,
                "[]",
                [&](const Cursor& cursor) -> Reply {
                    return to_raw(feature::to_locations(
                        feature::type_definition(project.peer_queries(), cursor),
                        server.position_encoding));
                });
        },
        {.closed = true});

    on_document<protocol::ImplementationParams>(
        [this](RequestContext& ctx, const auto& params, ResolvedDoc document) -> RawResult {
            auto& project = *document.project;
            co_return co_await navigate(
                project,
                document.ticket,
                document.path_id,
                params.position,
                "[]",
                [&](const Cursor& cursor) -> Reply {
                    return to_raw(feature::to_locations(
                        feature::implementation(project.peer_queries(), cursor),
                        server.position_encoding));
                });
        },
        {.closed = true});

    on_document<protocol::CallHierarchyPrepareParams>(
        [this](RequestContext& ctx, const auto& params, ResolvedDoc document) -> RawResult {
            auto& project = *document.project;
            co_return co_await navigate(project,
                                        document.ticket,
                                        document.path_id,
                                        params.position,
                                        "null",
                                        [&](const Cursor& cursor) -> Reply {
                                            auto items = feature::call_hierarchy_items(
                                                project.peer_queries(),
                                                cursor,
                                                server.position_encoding);
                                            if(items.empty()) {
                                                return codec::RawValue{"null"};
                                            }
                                            return to_raw(items);
                                        });
        },
        {.closed = true});

    on_document<protocol::CallHierarchyIncomingCallsParams>(
        [this](RequestContext& ctx, const auto& params, ResolvedDoc document) -> RawResult {
            auto& project = *document.project;
            auto symbol = item_symbol(project, document.path_id, params.item, "call hierarchy");
            if(!symbol) {
                co_await kota::fail(std::move(symbol.error()));
            }
            co_return to_raw(feature::incoming_calls(project.peer_queries(),
                                                     *symbol,
                                                     document.path_id,
                                                     server.position_encoding));
        },
        {.closed = true});

    on_document<protocol::CallHierarchyOutgoingCallsParams>(
        [this](RequestContext& ctx, const auto& params, ResolvedDoc document) -> RawResult {
            auto& project = *document.project;
            auto symbol = item_symbol(project, document.path_id, params.item, "call hierarchy");
            if(!symbol) {
                co_await kota::fail(std::move(symbol.error()));
            }
            co_return to_raw(feature::outgoing_calls(project.peer_queries(),
                                                     *symbol,
                                                     document.path_id,
                                                     server.position_encoding));
        },
        {.closed = true});

    on_document<protocol::TypeHierarchyPrepareParams>(
        [this](RequestContext& ctx, const auto& params, ResolvedDoc document) -> RawResult {
            auto& project = *document.project;
            co_return co_await navigate(project,
                                        document.ticket,
                                        document.path_id,
                                        params.position,
                                        "null",
                                        [&](const Cursor& cursor) -> Reply {
                                            auto items = feature::type_hierarchy_items(
                                                project.peer_queries(),
                                                cursor,
                                                server.position_encoding);
                                            if(items.empty()) {
                                                return codec::RawValue{"null"};
                                            }
                                            return to_raw(items);
                                        });
        },
        {.closed = true});

    on_document<protocol::TypeHierarchySupertypesParams>(
        [this](RequestContext& ctx, const auto& params, ResolvedDoc document) -> RawResult {
            auto& project = *document.project;
            auto symbol = item_symbol(project, document.path_id, params.item, "type hierarchy");
            if(!symbol) {
                co_await kota::fail(std::move(symbol.error()));
            }
            co_return to_raw(feature::supertypes(project.peer_queries(),
                                                 *symbol,
                                                 document.path_id,
                                                 server.position_encoding));
        },
        {.closed = true});

    on_document<protocol::TypeHierarchySubtypesParams>(
        [this](RequestContext& ctx, const auto& params, ResolvedDoc document) -> RawResult {
            auto& project = *document.project;
            auto symbol = item_symbol(project, document.path_id, params.item, "type hierarchy");
            if(!symbol) {
                co_await kota::fail(std::move(symbol.error()));
            }
            co_return to_raw(feature::subtypes(project.peer_queries(),
                                               *symbol,
                                               document.path_id,
                                               server.position_encoding));
        },
        {.closed = true});

    // workspace/symbol over every project: each project's ranked matches,
    // interleaved rank by rank, a symbol two projects index listed once.
    on_request<protocol::WorkspaceSymbolParams>(
        [this](RequestContext& ctx, const protocol::WorkspaceSymbolParams& params) -> RawResult {
            server.pool.foreground_pulse();
            // Another checkout of the same code opened beside it (a
            // worktree) holds a copy of each symbol at the same place in the
            // same text of its folder: the copy of a folder with open
            // documents, the one being edited, stands for them.
            auto ordered = server.projects;
            ranges::stable_partition(ordered, [&](const std::shared_ptr<ServedProject>& project) {
                return ranges::any_of(server.documents, [&](const auto& entry) {
                    return entry.second.owner == project.get();
                });
            });
            llvm::SmallVector<std::vector<feature::WorkspaceSymbol>> ranked;
            for(auto& project: ordered) {
                ranked.push_back(feature::workspace_symbols(project->peer_queries(),
                                                            params.query,
                                                            Spelling(project->project.root),
                                                            server.position_encoding));
            }
            co_return to_raw(feature::merge_workspace_symbols(ranked));
        });
}

}  // namespace clice

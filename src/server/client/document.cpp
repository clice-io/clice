module clice;

import :feature.diagnostics;
import :feature.document_links;
import :feature.document_symbols;
import :feature.folding_ranges;
import :feature.hover;
import :feature.inlay_hints;
import :feature.protocol;
import :feature.selection_ranges;
import :feature.semantic_tokens;
import :semantic.symbol;
import :server.client;
import :server.master;
import :server.served_project;
import :support.json;
import :worker.protocol;

namespace clice {

kota::task<codec::RawValue, ipc::Error> stop_reply(Stop stop) {
    if(stop.error) {
        co_await kota::fail(std::move(*stop.error));
    }
    co_return codec::RawValue{"null"};
}

/// The card the index gives for the symbol at a position of the document's
/// serving source (feature::hover_info).
static std::optional<feature::HoverInfo> index_hover_card(ServedProject& project,
                                                          const Session& session,
                                                          const protocol::Position& at) {
    auto cursor = project.cursor_at(session.path_id, at);
    if(!cursor) {
        return std::nullopt;
    }
    return feature::hover_info(project.peer_queries(),
                               *cursor,
                               project.project.settings.documentation);
}

LSPClient::RawResult LSPClient::send_tokens(ResolvedDoc document,
                                            kota::cancellation_token token,
                                            TokensRequest request) {
    auto& [path, path_id, session, served, version, ticket] = document;
    auto& query = served->project.index_query;
    std::optional<index::IndexSource> source;
    std::optional<std::vector<feature::SemanticToken>> tokens;
    switch(co_await served->pick_route(ticket, {.full_lex = true}, &source)) {
        case Route::Outdated: co_await kota::fail(content_modified());
        case Route::Index: {
            index_served.insert(path_id);
            tokens = feature::semantic_tokens(served->text_of(*session),
                                              query,
                                              *source,
                                              served->project.settings.semantic_tokens);
            break;
        }
        case Route::Empty: {
            // The client caches this null, and only a semanticTokens
            // refresh makes it re-pull once the cold document's shard
            // arrives — the merge refreshes the documents served from the
            // index, so an empty pull registers the same interest as a
            // served one.
            index_served.insert(path_id);
            co_return codec::RawValue{"null"};
        }
        case Route::Ast: {
            tokens = co_await served
                         ->ask_worker(ticket,
                                      feature::RequestKind::SemanticTokens,
                                      "SemanticTokens",
                                      worker::SemanticTokensParams{
                                          .settings = served->project.settings,
                                      },
                                      std::move(token))
                         .or_fail();
            break;
        }
    }
    if(!tokens) {
        co_return codec::RawValue{"null"};
    }
    if(auto preamble = query.preamble_index(path_id)) {
        feature::merge_preamble_macros(*preamble, *tokens);
    }
    // Encoded against the document's text right away.
    auto encoding = server.position_encoding;
    auto positions = session->positions();
    if(request.window) {
        auto window = positions.offset_range(*request.window, encoding);
        co_return to_raw(feature::to_protocol(*tokens, positions, encoding, token_legend, window));
    }
    auto encoded = feature::to_protocol(*tokens, positions, encoding, token_legend);
    next_result_id += 1;
    encoded.result_id = std::to_string(next_result_id);
    auto& sent = sent_tokens[path_id];
    bool held = request.previous && sent.result_id == *request.previous;
    auto previous = std::exchange(sent, {*encoded.result_id, encoded.data});
    if(held) {
        co_return to_raw(protocol::SemanticTokensDelta{
            .result_id = encoded.result_id,
            .edits = feature::semantic_tokens_edits(previous.data, encoded.data),
        });
    }
    co_return to_raw(encoded);
}

void LSPClient::register_document_requests() {
    // Whole-document features and hover are routed by readiness
    // (ServedProject::pick_route): the AST answers when current, the results
    // computed from the index answer while it is not, and a document the
    // policy keeps un-compiled answers with its pinned degraded surface
    // (empty inlay hints). Each forwards the request's cancellation token
    // to the worker sends (ServedProject::ask_worker).
    on_document<protocol::HoverParams>([this](RequestContext& ctx,
                                              const auto& params,
                                              ResolvedDoc document) -> RawResult {
        auto& [path, path_id, session, served, version, ticket] = document;
        auto& project = *served;
        auto& query = project.project.index_query;
        auto encoding = server.position_encoding;
        auto& position = params.position;

        auto card = [&](const feature::HoverInfo& info) {
            return to_raw(feature::to_protocol(info, hover_markup, session->positions(), encoding));
        };

        // A directive's card names its target, which the worker knows only by
        // identity: this side answers it, from the links. Off a directive
        // line there is none to look for.
        auto offset = session->positions().offset(position, encoding);
        bool on_directive =
            offset &&
            feature::on_directive_line(session->text(), *offset, project.dialect(path_id));
        auto directive_card =
            [&](llvm::ArrayRef<index::DocumentLink> links) -> std::optional<codec::RawValue> {
            auto* link = feature::link_at(links, *offset);
            if(!link) {
                return std::nullopt;
            }
            auto hover = feature::directive_hover(session->text(),
                                                  session->positions(),
                                                  project.project.file_table,
                                                  *link,
                                                  hover_markup,
                                                  encoding);
            return hover ? to_raw(*hover) : codec::RawValue{"null"};
        };

        switch(co_await project.pick_route(ticket, {})) {
            case Route::Outdated: co_await kota::fail(content_modified());
            case Route::Index: {
                if(on_directive) {
                    if(auto shown = directive_card(
                           feature::document_links(project.text_of(*session), query, path_id))) {
                        index_served.insert(path_id);
                        co_return std::move(*shown);
                    }
                }
                auto info = index_hover_card(project, *session, position);
                if(!info) {
                    co_return codec::RawValue{"null"};
                }
                // Without the definition, the index card tells little past the
                // name (the entity the buffer's index recorded can be gone, as
                // after an imported interface changed): while a compile can
                // run, the AST's card is worth the wait.
                if(info->definition.empty() && info->file.empty() &&
                   project.serving_of(*session) == ServingMode::Escalated &&
                   !project.compile_stopped(*session)) {
                    break;
                }
                index_served.insert(path_id);
                co_return card(*info);
            }
            case Route::Empty: co_return codec::RawValue{"null"};
            case Route::Ast: break;
        }

        // A module name's card names the unit defining the module, which the
        // worker cannot see across files: this side answers it, from the
        // index.
        if(auto cursor = project.cursor_at(path_id, position)) {
            auto info = query.symbol_info(cursor->symbols.front(), cursor->site.file);
            if(info && info->kind == SymbolKind::Module) {
                co_return card(feature::module_hover(project.peer_queries(), *cursor, *info));
            }
        }

        if(on_directive) {
            auto links = co_await project.directive_links(ticket, ctx.cancellation).or_fail();
            if(auto shown = directive_card(links)) {
                co_return std::move(*shown);
            }
        }

        auto info =
            co_await project
                .ask_worker(ticket,
                            feature::RequestKind::Hover,
                            "Hover",
                            worker::HoverParams{
                                .offset = session->positions().offset_clamped(position, encoding),
                                .settings = project.project.settings,
                            },
                            ctx.cancellation)
                .or_fail();
        if(info) {
            co_return card(*info);
        }
        // The preamble region is the one place the AST's having no card is
        // not authoritative — it is compiled into the PCH and invisible to
        // the worker (a `#define` there has no node). The index card fills
        // that gap, and only that gap: past the bound the AST saw the code
        // and declined on purpose (a lambda's auto parameter must not hover,
        // and the index would happily name it `auto:1`).
        if(auto compiled = project.current(path_id) ? project.latest(path_id) : nullptr;
           compiled && compiled->loaded_pch()) {
            auto& pch_cache = project.project.pch_cache;
            if(auto it = pch_cache.find(*compiled->loaded_pch());
               it != pch_cache.end() && offset && *offset < it->second.bound) {
                if(auto info = index_hover_card(project, *session, position)) {
                    co_return card(*info);
                }
            }
        }
        co_return codec::RawValue{"null"};
    });

    on_document<protocol::SemanticTokensParams>(
        [this](RequestContext& ctx, const auto& params, ResolvedDoc document) {
            return send_tokens(std::move(document), ctx.cancellation, {});
        });

    on_document<protocol::SemanticTokensDeltaParams>(
        [this](RequestContext& ctx, const auto& params, ResolvedDoc document) {
            return send_tokens(std::move(document),
                               ctx.cancellation,
                               {.previous = params.previous_result_id});
        });

    on_document<protocol::SemanticTokensRangeParams>(
        [this](RequestContext& ctx, const auto& params, ResolvedDoc document) {
            return send_tokens(std::move(document), ctx.cancellation, {.window = params.range});
        });

    on_document<protocol::InlayHintParams>([this](RequestContext& ctx,
                                                  const auto& params,
                                                  ResolvedDoc document) -> RawResult {
        auto& [path, path_id, session, served, version, ticket] = document;
        auto encoding = server.position_encoding;
        // Inlay hints are Sema products the index cannot compute; a document
        // the policy keeps un-compiled answers honestly empty. The compile
        // that follows an escalation pushes an inlayHint refresh, so clients
        // re-pull once real hints exist.
        if(served->serving_of(*session) == ServingMode::IndexOnly) {
            index_served.insert(path_id);
            co_return codec::RawValue{"[]"};
        }
        // Clamped against the buffer the ticket was taken on: a buffer that
        // moves before the reply arrives turns the reply into ContentModified.
        auto hints =
            co_await served
                ->ask_worker(ticket,
                             feature::RequestKind::InlayHint,
                             "InlayHint",
                             worker::InlayHintParams{
                                 .range = session->positions().offset_range(params.range, encoding),
                                 .settings = served->project.settings,
                             },
                             ctx.cancellation)
                .or_fail();
        if(!label_parts) {
            co_return to_raw(feature::to_protocol(hints, session->positions(), encoding));
        }
        co_return to_raw(feature::to_protocol(hints,
                                              session->positions(),
                                              encoding,
                                              served->peer_queries(),
                                              path_id));
    });

    on_document<protocol::FoldingRangeParams>(
        [this](RequestContext& ctx, const auto& params, ResolvedDoc document) -> RawResult {
            auto& [path, path_id, session, served, version, ticket] = document;
            auto convert = [&](llvm::ArrayRef<feature::FoldingRange> folds) {
                return to_raw(feature::to_protocol(folds,
                                                   session->text(),
                                                   session->positions(),
                                                   server.position_encoding,
                                                   folding_client));
            };
            std::optional<index::IndexSource> source;
            switch(co_await served->pick_route(ticket, {.full_lex = true}, &source)) {
                case Route::Outdated: co_await kota::fail(content_modified());
                case Route::Index: {
                    auto folds = feature::folding_ranges(served->text_of(*session),
                                                         served->project.index_query,
                                                         *source);
                    index_served.insert(path_id);
                    co_return convert(folds);
                }
                case Route::Empty: {
                    // Same contract as the semantic-tokens Empty route: the
                    // client caches this reply, and only a foldingRange
                    // refresh makes it re-pull once the cold document's
                    // shard arrives.
                    index_served.insert(path_id);
                    co_return codec::RawValue{"[]"};
                }
                case Route::Ast: break;
            }
            auto folds = co_await served
                             ->ask_worker(ticket,
                                          feature::RequestKind::FoldingRange,
                                          "FoldingRange",
                                          worker::FoldingRangeParams{},
                                          ctx.cancellation)
                             .or_fail();
            if(!folds) {
                co_return codec::RawValue{"null"};
            }
            co_return convert(*folds);
        });

    // One chain of selection ranges per position: from the AST, or for a
    // document without one from the raw lex of its text.
    on_document<protocol::SelectionRangeParams>([this](RequestContext& ctx,
                                                       const auto& params,
                                                       ResolvedDoc document) -> RawResult {
        auto& [path, path_id, session, served, version, ticket] = document;
        auto encoding = server.position_encoding;
        std::vector<std::uint32_t> offsets;
        auto map = session->positions();
        for(const auto& position: params.positions) {
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
            return convert(feature::lexical_selection_ranges(served->text_of(*session), offsets));
        };

        switch(co_await served->pick_route(ticket, {})) {
            case Route::Outdated: co_await kota::fail(content_modified());
            case Route::Index:
            case Route::Empty: co_return lexical();
            case Route::Ast: break;
        }
        auto chains = co_await served
                          ->ask_worker(ticket,
                                       feature::RequestKind::SelectionRange,
                                       "SelectionRange",
                                       worker::SelectionRangeParams{.offsets = offsets},
                                       ctx.cancellation)
                          .or_fail();
        if(chains.empty()) {
            co_return lexical();
        }
        co_return convert(chains);
    });

    // A flat reply places every symbol at the document's URI as the request
    // names it.
    on_document<protocol::DocumentSymbolParams>([this](RequestContext& ctx,
                                                       const auto& params,
                                                       ResolvedDoc document) -> RawResult {
        auto& [path, path_id, session, served, version, ticket] = document;
        auto encoding = server.position_encoding;
        auto convert = [&](llvm::ArrayRef<feature::DocumentSymbol> symbols) {
            if(symbol_client.hierarchical) {
                return to_raw(
                    feature::to_protocol(symbols, session->positions(), encoding, symbol_client));
            }
            return to_raw(feature::to_symbol_information(symbols,
                                                         session->positions(),
                                                         encoding,
                                                         symbol_client,
                                                         params.text_document.uri));
        };
        std::optional<index::IndexSource> source;
        switch(co_await served->pick_route(ticket,
                                           {.full_lex = true, .await_cold_attempt = true},
                                           &source)) {
            case Route::Outdated: co_await kota::fail(content_modified());
            case Route::Index: {
                auto symbols = feature::document_symbols(served->text_of(*session),
                                                         served->project.index_query,
                                                         *source,
                                                         served->project.settings.document_symbols);
                index_served.insert(path_id);
                co_return convert(symbols);
            }
            case Route::Empty: co_return codec::RawValue{"[]"};
            case Route::Ast: break;
        }
        auto symbols = co_await served
                           ->ask_worker(ticket,
                                        feature::RequestKind::DocumentSymbol,
                                        "DocumentSymbol",
                                        worker::DocumentSymbolParams{
                                            .settings = served->project.settings,
                                        },
                                        ctx.cancellation)
                           .or_fail();
        if(!symbols) {
            co_return codec::RawValue{"null"};
        }
        co_return convert(*symbols);
    });

    // The worker's focus-file links merged behind the PCH's cached preamble
    // links.
    on_document<protocol::DocumentLinkParams>(
        [this](RequestContext& ctx, const auto& params, ResolvedDoc document) -> RawResult {
            auto& [path, path_id, session, served, version, ticket] = document;
            auto encoding = server.position_encoding;
            auto& files = served->project.file_table;
            switch(co_await served->pick_route(ticket, {.await_cold_attempt = true})) {
                case Route::Outdated: co_await kota::fail(content_modified());
                case Route::Index: {
                    // Manifest edges cover the whole document (the background
                    // index has no preamble split); guard-skipped lines and
                    // __has_include/#embed have no edge and produce no link.
                    auto links = feature::document_links(served->text_of(*session),
                                                         served->project.index_query,
                                                         path_id);
                    index_served.insert(path_id);
                    co_return to_raw(
                        feature::to_protocol(links, session->positions(), encoding, files));
                }
                case Route::Empty: co_return codec::RawValue{"[]"};
                case Route::Ast: break;
            }
            auto links = co_await served->directive_links(ticket, ctx.cancellation).or_fail();
            co_return to_raw(feature::to_protocol(links, session->positions(), encoding, files));
        });

    // The diagnostics of the document's buffer as it is now: the pull awaits
    // the compile the buffer is owed in the project the document belongs
    // to, following it to another project and an edit arriving meanwhile
    // rather than answering ContentModified — a pull names no version, and
    // clients clear what they hold on that error. Following has no bound of
    // its own: a client that no longer wants the answer cancels the pull. A
    // document the policy keeps un-compiled waits for nothing. A pull is
    // always answered with a report, never an error: on an error, clients
    // clear what they hold or stop pulling the document.
    on_document<protocol::DocumentDiagnosticParams>(
        [this](RequestContext& ctx, const auto& params, ResolvedDoc document) -> RawResult {
            auto& [path, path_id, session, served, version, ticket] = document;
            std::vector<feature::FileDiagnostic> items;
            while(session && !session->closed) {
                auto* open = server.find_document(path_id);
                if(!open || open->session != session || !open->owner) {
                    break;
                }
                auto owner = open->owner->shared_from_this();
                if(owner->serving_of(*session) == ServingMode::Escalated) {
                    auto taken = Ticket::take(session);
                    co_await owner->ensure_compiled(session);
                    if(!taken.fresh()) {
                        continue;
                    }
                }
                items = owner->finished_diagnostics(*session);
                break;
            }
            // A second name answers the warning once its text parted from
            // the first name's, before the pull or while it waited.
            auto* second = second_name(path);
            auto shown = second ? server.find_session(second->buffer.path_id) : nullptr;
            if(second && (!shown || second->buffer.text() != shown->text())) {
                co_return full_report(
                    to_client({feature::file_warning(divergence_message(*second))},
                              path,
                              second->buffer.positions()));
            }
            // A pull's diagnostics are of the session's text
            // (finished_diagnostics).
            auto report = full_report(session ? to_client(items, path, session->positions())
                                              : std::vector<protocol::Diagnostic>());
            if(session) {
                // Recorded against the live session: one that replaced this
                // pull's meanwhile (a reopen) compiles the same text, and its
                // commit must correct the empty answer the closed one gave.
                if(auto live = server.find_session(path_id)) {
                    pulled[path_id] = {
                        .version = live->version,
                        .report = llvm::xxh3_64bits(report.data),
                    };
                }
            }
            co_return report;
        },
        {.closed = true});
}

}  // namespace clice

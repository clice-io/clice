module clice;

import :compile.inactive_regions;
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

MasterServer::SecondName* LSPClient::second_name(llvm::StringRef spelling) {
    auto path_id =
        spelling.empty() ? std::nullopt : server.files.find(Spelling::absolute(spelling));
    return path_id ? server.find_second_name(*path_id, spelling) : nullptr;
}

LSPClient::ResolvedDoc LSPClient::resolve_uri(const std::string& uri) {
    auto path = feature::to_path(uri);
    if(!path) {
        // Not a file: no document of ours.
        return ResolvedDoc{.project = server.projects.front()};
    }
    auto path_id = server.files.intern(*path);
    auto project = server.owner_of(path_id).shared_from_this();
    auto session = server.find_session(path_id);
    auto version = session ? std::optional(session->version) : std::nullopt;
    // A document under a second name has a buffer of its own: it is the
    // file — its session, its index — only while both hold the same text.
    if(auto* second = server.find_second_name(path_id, *path)) {
        if(server.find_document(path_id)->diverged(*second)) {
            return ResolvedDoc{.path = std::move(*path).str(), .project = std::move(project)};
        }
        version = second->buffer.version;
    }
    return ResolvedDoc{
        .path = std::move(*path).str(),
        .path_id = path_id,
        .project = std::move(project),
        .version = version,
        .ticket = Ticket::take(std::move(session)),
    };
}

async::task<Route> LSPClient::pick_route(const ResolvedDoc& document,
                                         RouteOptions options,
                                         std::optional<index::IndexSource>* source) {
    auto route = co_await document.project->pick_route(document.ticket, options, source);
    // The client caches an empty reply too, and only a refresh makes it
    // pull again once the document's shard or compile arrives.
    if(route == Route::Index || route == Route::Empty) {
        index_served.insert(document.path_id);
    }
    co_return route;
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
    return feature::hover_info(project.query_all(),
                               *cursor,
                               project.project.settings.documentation);
}

/// The code the AST's context skipped, when the index serving the document
/// holds the entries of the file's other contexts there and the buffer is
/// not too large to lex again: the index completes the AST's answer there,
/// `source` set to it. Empty otherwise.
static llvm::ArrayRef<std::uint32_t> other_contexts(const ServedProject& served,
                                                    const Session& session,
                                                    std::optional<index::IndexSource>& source) {
    if(session.text().size() > max_lexed_buffer) {
        return {};
    }
    auto skipped = served.skipped_code(session.path_id);
    if(!skipped.indexed) {
        return {};
    }
    source = served.query().serving(session.path_id);
    return skipped.regions;
}

LSPClient::RawResult LSPClient::send_tokens(ResolvedDoc document,
                                            async::cancellation_token token,
                                            TokensRequest request) {
    auto& [path, path_id, served, version, ticket] = document;
    auto& session = ticket.session;
    std::optional<index::IndexSource> source;
    auto from_index = [&](llvm::ArrayRef<std::uint32_t> only_in) {
        return feature::semantic_tokens(served->text_of(*session),
                                        served->query(),
                                        *source,
                                        served->project.settings.semantic_tokens,
                                        only_in);
    };
    std::vector<feature::SemanticToken> tokens;
    switch(co_await pick_route(document, {.lexes_whole_buffer = true}, &source)) {
        case Route::Outdated: co_await async::fail(content_modified());
        case Route::Index: {
            tokens = from_index({});
            break;
        }
        case Route::Empty: co_return codec::raw_value{"null"};
        case Route::AST: {
            auto ast = co_await served
                           ->ast_request(ticket,
                                         feature::RequestKind::SemanticTokens,
                                         worker::SemanticTokensParams{
                                             .settings = served->project.settings,
                                         },
                                         std::move(token))
                           .or_fail();
            if(!ast) {
                co_return codec::raw_value{"null"};
            }
            tokens = std::move(*ast);
            if(auto skipped = other_contexts(*served, *session, source); !skipped.empty()) {
                tokens = feature::merge(std::move(tokens), from_index(skipped), skipped);
            }
            break;
        }
    }
    if(auto preamble = served->query().preamble_index(path_id)) {
        feature::merge_preamble_macros(*preamble, tokens);
    }
    // Encoded against the document's text right away.
    auto encoding = server.position_encoding;
    auto positions = session->positions();
    if(request.window) {
        auto window = positions.offset_range(*request.window, encoding);
        co_return to_raw(feature::to_protocol(tokens, positions, encoding, token_legend, window));
    }
    auto encoded = feature::to_protocol(tokens, positions, encoding, token_legend);
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
    // computed from the index answer while it is not, and a document that
    // is not compiled answers what the index gives (empty inlay hints).
    // Each forwards the request's cancellation token to the worker sends
    // (ServedProject::ast_request).
    on_document<protocol::HoverParams>([this](RequestContext& ctx,
                                              const auto& params,
                                              ResolvedDoc document) -> RawResult {
        auto& [path, path_id, served, version, ticket] = document;
        auto& session = ticket.session;
        auto& project = *served;
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
            [&](llvm::ArrayRef<feature::DocumentLink> links) -> std::optional<codec::raw_value> {
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
            return hover ? to_raw(*hover) : codec::raw_value{"null"};
        };

        switch(co_await pick_route(document, {})) {
            case Route::Outdated: co_await async::fail(content_modified());
            case Route::Index: {
                if(on_directive) {
                    if(auto shown =
                           directive_card(feature::document_links(project.text_of(*session),
                                                                  project.query(),
                                                                  path_id))) {
                        co_return std::move(*shown);
                    }
                }
                auto info = index_hover_card(project, *session, position);
                if(!info) {
                    co_return codec::raw_value{"null"};
                }
                // Without the definition, the index card tells little past the
                // name (the entity the buffer's index recorded can be gone, as
                // after an imported interface changed): while a compile can
                // run, the AST's card is worth the wait.
                if(info->definition.empty() && info->file.empty() && project.compiles(*session) &&
                   !project.compile_stopped(*session)) {
                    break;
                }
                co_return card(*info);
            }
            case Route::Empty: co_return codec::raw_value{"null"};
            case Route::AST: break;
        }

        // A module name's card names the unit defining the module, which the
        // worker cannot see across files: this side answers it, from the
        // index.
        if(auto cursor = project.cursor_at(path_id, position)) {
            auto query = project.query_all();
            auto info = query.symbol_info(cursor->symbols.front(), cursor->site.file);
            if(info && info->kind == SymbolKind::Module) {
                co_return card(feature::module_hover(query,
                                                     *cursor,
                                                     *info,
                                                     project.project.settings.documentation));
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
                .ast_request(ticket,
                             feature::RequestKind::Hover,
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
            if(auto* pch = project.project.built_pch(*compiled->loaded_pch());
               pch && offset && *offset < pch->bound) {
                if(auto info = index_hover_card(project, *session, position)) {
                    co_return card(*info);
                }
            }
        }
        // Code the compile's context skipped: another context of the file
        // compiles it, and the index has its card.
        if(auto skipped = project.skipped_code(path_id);
           offset && skipped.indexed && inactive_at(skipped.regions, *offset)) {
            if(auto info = index_hover_card(project, *session, position)) {
                co_return card(*info);
            }
        }
        co_return codec::raw_value{"null"};
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

    on_document<protocol::InlayHintParams>(
        [this](RequestContext& ctx, const auto& params, ResolvedDoc document) -> RawResult {
            auto& [path, path_id, served, version, ticket] = document;
            auto& session = ticket.session;
            auto encoding = server.position_encoding;
            auto route = co_await pick_route(document, {.needs_ast = true});
            if(route == Route::Outdated) {
                co_await async::fail(content_modified());
            }
            // Inlay hints are Sema products the index cannot compute; a document
            // that is not compiled answers honestly empty. Its first compile,
            // once it starts compiling, pushes an inlayHint refresh, so clients
            // re-pull once real hints exist.
            if(route == Route::Empty) {
                co_return codec::raw_value{"[]"};
            }
            // Clamped against the buffer the ticket was taken on: a buffer that
            // moves before the reply arrives turns the reply into ContentModified.
            auto hints =
                co_await served
                    ->ast_request(
                        ticket,
                        feature::RequestKind::InlayHint,
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
                                                  served->query_all(),
                                                  path_id));
        });

    on_document<protocol::FoldingRangeParams>(
        [this](RequestContext& ctx, const auto& params, ResolvedDoc document) -> RawResult {
            auto& [path, path_id, served, version, ticket] = document;
            auto& session = ticket.session;
            auto convert = [&](llvm::ArrayRef<feature::FoldingRange> folds) {
                return to_raw(feature::to_protocol(folds,
                                                   session->text(),
                                                   session->positions(),
                                                   server.position_encoding,
                                                   folding_client));
            };
            std::optional<index::IndexSource> source;
            auto from_index = [&] {
                return feature::folding_ranges(served->text_of(*session), served->query(), *source);
            };
            switch(co_await pick_route(document, {.lexes_whole_buffer = true}, &source)) {
                case Route::Outdated: co_await async::fail(content_modified());
                case Route::Index: co_return convert(from_index());
                case Route::Empty: co_return codec::raw_value{"[]"};
                case Route::AST: break;
            }
            auto folds = co_await served
                             ->ast_request(ticket,
                                           feature::RequestKind::FoldingRange,
                                           worker::FoldingRangeParams{},
                                           ctx.cancellation)
                             .or_fail();
            if(!folds) {
                co_return codec::raw_value{"null"};
            }
            if(auto skipped = other_contexts(*served, *session, source); !skipped.empty()) {
                co_return convert(feature::merge(std::move(*folds), from_index(), skipped));
            }
            co_return convert(*folds);
        });

    // One chain of selection ranges per position: from the AST, or for a
    // document without one from the raw lex of its text.
    on_document<protocol::SelectionRangeParams>([this](RequestContext& ctx,
                                                       const auto& params,
                                                       ResolvedDoc document) -> RawResult {
        auto& [path, path_id, served, version, ticket] = document;
        auto& session = ticket.session;
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
            if(session->text().size() > max_lexed_buffer) {
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

        switch(co_await pick_route(document, {})) {
            case Route::Outdated: co_await async::fail(content_modified());
            case Route::Index:
            case Route::Empty: co_return lexical();
            case Route::AST: break;
        }
        auto chains = co_await served
                          ->ast_request(ticket,
                                        feature::RequestKind::SelectionRange,
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
        auto& [path, path_id, served, version, ticket] = document;
        auto& session = ticket.session;
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
        auto from_index = [&] {
            return feature::document_symbols(served->text_of(*session),
                                             served->query(),
                                             *source,
                                             served->project.settings.document_symbols);
        };
        switch(co_await pick_route(document,
                                   {.lexes_whole_buffer = true, .waits_for_index = true},
                                   &source)) {
            case Route::Outdated: co_await async::fail(content_modified());
            case Route::Index: co_return convert(from_index());
            case Route::Empty: co_return codec::raw_value{"[]"};
            case Route::AST: break;
        }
        auto symbols = co_await served
                           ->ast_request(ticket,
                                         feature::RequestKind::DocumentSymbol,
                                         worker::DocumentSymbolParams{
                                             .settings = served->project.settings,
                                         },
                                         ctx.cancellation)
                           .or_fail();
        if(!symbols) {
            co_return codec::raw_value{"null"};
        }
        if(auto skipped = other_contexts(*served, *session, source); !skipped.empty()) {
            co_return convert(feature::merge(std::move(*symbols), from_index(), skipped));
        }
        co_return convert(*symbols);
    });

    // The worker's focus-file links merged behind the PCH's cached preamble
    // links.
    on_document<protocol::DocumentLinkParams>(
        [this](RequestContext& ctx, const auto& params, ResolvedDoc document) -> RawResult {
            auto& [path, path_id, served, version, ticket] = document;
            auto& session = ticket.session;
            auto encoding = server.position_encoding;
            auto& files = served->project.file_table;
            switch(co_await pick_route(document, {.waits_for_index = true})) {
                case Route::Outdated: co_await async::fail(content_modified());
                case Route::Index: {
                    // Manifest edges cover the whole document (the background
                    // index has no preamble split); guard-skipped lines and
                    // __has_include/#embed have no edge and produce no link.
                    auto links = feature::document_links(served->text_of(*session),
                                                         served->query(),
                                                         path_id);
                    co_return to_raw(
                        feature::to_protocol(links, session->positions(), encoding, files));
                }
                case Route::Empty: co_return codec::raw_value{"[]"};
                case Route::AST: break;
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
    // document that is not compiled waits for nothing. A pull is
    // always answered with a report, never an error: on an error, clients
    // clear what they hold or stop pulling the document.
    on_document<protocol::DocumentDiagnosticParams>(
        [this](RequestContext& ctx, const auto& params, ResolvedDoc document) -> RawResult {
            auto& [path, path_id, served, version, ticket] = document;
            auto& session = ticket.session;
            // The pull follows the document, not the project it belonged
            // to: held, a removed folder's project would keep the project
            // taking its root over from starting, and the pull would wait
            // for that one's compile.
            served.reset();
            auto report = full_report(std::vector<protocol::Diagnostic>());
            while(session && !session->closed) {
                auto* open = server.find_document(path_id);
                if(!open || open->session != session || !open->owner) {
                    break;
                }
                auto owner = open->owner->shared_from_this();
                if(owner->compiles(*session)) {
                    auto taken = Ticket::take(session);
                    co_await owner->ensure_compiled(session, {.ast = false});
                    if(!taken.fresh()) {
                        continue;
                    }
                }
                report = pull_report(*owner, *session, path);
                break;
            }
            // A second name answers the warning once its text parted from
            // the first name's, before the pull or while it waited.
            auto* second = second_name(path);
            if(second && server.find_document(second->buffer.path_id)->diverged(*second)) {
                co_return full_report(
                    to_client({feature::file_warning(divergence_message(*second))},
                              path,
                              second->buffer.positions()));
            }
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

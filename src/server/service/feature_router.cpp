#include "server/service/feature_router.h"

#include <algorithm>
#include <format>
#include <iterator>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "command/search_config.h"
#include "sched/context.h"
#include "sched/index/pump.h"
#include "semantic/symbol.h"
#include "server/service/ast_family.h"
#include "server/service/worker_forwarder.h"
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

bool FeatureRouter::ast_answerable(const Session& session) const {
    return ast.projections.index_current(session.path_id) && !session.quarantine.blocked();
}

/// The rows that may serve the session's document from the index: its own
/// file index when current (the quarantine fallback — the worker cannot
/// be asked, the rows are still true), else the disk shard admitted by
/// freshness clause 4. Content is the buffer either way. The projection
/// shared_ptr keeps a family-side replacement from freeing the rows under
/// the caller.
struct IndexRowsSource {
    std::shared_ptr<const ASTProjection> projection;
    const index::Shard* shard = nullptr;

    explicit operator bool() const {
        return shard != nullptr;
    }
};

static IndexRowsSource index_rows_source(const IndexQuery& query,
                                         const ASTProjectionTable& table,
                                         const Session& session) {
    if(table.index_current(session.path_id)) {
        auto projection = table.projection(session.path_id);
        const auto& rows = projection->file_rows();
        return {std::move(projection), &rows};
    }
    return {nullptr, query.open_session_shard(session)};
}

kota::task<FeatureRouter::Route> FeatureRouter::pick_route(std::shared_ptr<Session> session,
                                                           bool full_lex,
                                                           IndexRowsSource* source) {
    // This handler is resumed eagerly: drain the transport pipe before
    // reading any buffer state, so a queued didChange or cancel lands
    // first (the same discipline as completion's yield).
    auto gen = session->generation;
    co_await kota::yield();
    if(session->generation != gen) {
        co_return Route::Superseded;
    }
    if(ast_answerable(*session)) {
        co_return Route::Ast;
    }
    // An oversized buffer is not worth a synchronous main-thread lex; the
    // full-lex projections follow the investment policy instead of the
    // index slice. Row-backed answers serve at any size.
    constexpr std::size_t full_lex_cap = 8 * 1024 * 1024;
    bool capped = full_lex && session->text.size() > full_lex_cap;
    if(!capped) {
        if(auto rows = index_rows_source(index_query, ast.projections, *session)) {
            // An index answer must not strand an escalated session's pending
            // build: this request still pulls the compile it would otherwise
            // have waited on, just without blocking.
            if(session->serving == ServingMode::Escalated &&
               !ast.projections.current(session->path_id)) {
                ast.request_compile(session);
            }
            if(source) {
                *source = std::move(rows);
            }
            co_return Route::Index;
        }
    }
    co_return session->serving == ServingMode::Escalated ? Route::Ast : Route::Empty;
}

FeatureRouter::IndexRows FeatureRouter::extract_rows(const index::Shard& shard) {
    IndexRows rows;
    shard.for_each_occurrence([&](const index::Occurrence& occurrence) {
        rows.occurrences.push_back(occurrence);
        return true;
    });
    shard.for_each_relation([&](index::SymbolHash hash, const index::Relation& relation) {
        RelationKind kind(relation.kind);
        if(kind.isDeclOrDef()) {
            auto copy = relation;
            rows.decls.push_back({.range = relation.range,
                                  .extent = copy.definition_range(),
                                  .symbol = hash,
                                  .definition = kind.is_one_of(RelationKind::Definition)});
        }
        return true;
    });
    return rows;
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

const clang::LangOptions& FeatureRouter::index_lang_options(const Session& session) {
    auto path = workspace.path_pool.resolve(session.path_id);
    auto own = command_lang(workspace, path);
    if(own && own->forces_c) {
        return feature::index_lang_options("", *own->forces_c, own->standard);
    }

    // A header's active context (the user's persisted choice, else the
    // resolved host) names the view being read; its command beats the
    // contributor union the way it does for the AST after an escalation.
    auto host = no_path_id;
    if(auto it = contexts.saved_contexts.find(session.path_id);
       it != contexts.saved_contexts.end()) {
        host = it->second.host_path_id;
    }
    if(host == no_path_id) {
        if(const auto* context = contexts.header_context(session.path_id)) {
            host = context->host_path_id;
        }
    }
    if(host != no_path_id) {
        auto host_path = workspace.path_pool.resolve(host);
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
                  llvm::all_of(llvm::make_first_range(it->second), [&](std::uint32_t tu) {
                      return workspace.path_pool.resolve(tu).ends_with(".c");
                  });
    return feature::index_lang_options(path,
                                       c_rows,
                                       own ? llvm::StringRef(own->standard) : llvm::StringRef());
}

std::optional<feature::IndexSymbolInfo> FeatureRouter::resolve_symbol_info(index::SymbolHash hash) {
    feature::IndexSymbolInfo info;
    if(index_query.find_symbol_info(hash, info.name, info.kind)) {
        return info;
    }
    return std::nullopt;
}

kota::task<bool> FeatureRouter::nav_gate(std::shared_ptr<Session> session) {
    switch(co_await pick_route(session, /*full_lex=*/false)) {
        case Route::Superseded: co_return false;
        // The index sources resolve the cursor under clauses 1/4 — or
        // reject it, which the query layers answer as empty. Either way
        // no compile is owed.
        case Route::Index:
        case Route::Empty: co_return true;
        case Route::Ast: break;
    }
    // Same posture as every AST-backed request: the session's file index
    // is produced by the very compile awaited here, so once this settles
    // the index describes the buffer. A failed or superseded compile
    // (buffer changed while awaiting) yields null rather than a lookup
    // against positions the buffer no longer has.
    auto gen = session->generation;
    if(!co_await ast.ensure_compiled(session) || session->generation != gen) {
        co_return false;
    }
    co_return true;
}

std::vector<feature::DocumentLink> FeatureRouter::find_preamble_links(const Session& session) {
    auto projection = ast.projections.projection(session.path_id);
    if(!projection || !projection->pch_key)
        return {};
    auto state = workspace.preamble_state(*projection->pch_key);
    if(!state)
        return {};
    // Link offsets are buffer coordinates as of the PCH build; serve them
    // only while the buffer still starts with that exact preamble text
    // (a deferred rebuild mid-edit keeps an old blob for a moved buffer).
    if(!state->matches_prefix(session.text))
        return {};
    return state->links();
}

std::vector<protocol::Location>
    FeatureRouter::resolve_directive_definition(Session& session,
                                                const protocol::Position& position) {
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
    FeatureRouter::resolve_preamble_hover(Session& session, const protocol::Position& position) {
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
    FeatureRouter::document_links(std::shared_ptr<Session> session,
                                  std::optional<kota::cancellation_token> token) {
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
        switch(co_await pick_route(session, /*full_lex=*/false)) {
            case Route::Superseded: co_return std::vector<protocol::DocumentLink>{};
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
                auto it = workspace.shards.find(session->path_id);
                if(it != workspace.shards.end() && it->second.matches_content(session->text)) {
                    auto raw = feature::index_document_links(session->text,
                                                             index_lang_options(*session),
                                                             index_query.include_edges(*session));
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
                    auto gen = session->generation;
                    co_await pump.await_attempt(session->path_id);
                    if(session->generation == gen) {
                        continue;
                    }
                }
                co_return std::vector<protocol::DocumentLink>{};
            }
            case Route::Ast: break;
        }
        break;
    }

    auto result = co_await forwarder.forward_document_links(session, std::move(token));
    if(!result.has_value())
        co_return kota::outcome_error(std::move(result.error()));

    // The preamble is compiled into the PCH, so the worker's AST only
    // covers the rest of the file — merge the preamble's links in front.
    std::vector<protocol::DocumentLink> links;
    convert(find_preamble_links(*session), links);
    convert(result.value(), links);
    co_return links;
}

kota::task<kota::codec::RawValue, kota::ipc::Error>
    FeatureRouter::definition(std::shared_ptr<Session> session,
                              llvm::StringRef path,
                              const protocol::Position& pos,
                              std::optional<kota::cancellation_token> token) {
    if(session && !co_await nav_gate(session)) {
        co_return serde_raw{"null"};
    }

    // Preamble include lines first: they have no symbol occurrence in
    // the index and are invisible to the worker's AST. A projection still
    // not current after the awaited compile means the world was re-dirtied
    // mid-flight (the round landed as bounded staleness): the cached
    // links may describe a pre-edit preamble — skip, and let the index and
    // worker paths below answer.
    if(session && ast.projections.current(session->path_id)) {
        if(auto directive = resolve_directive_definition(*session, pos); !directive.empty()) {
            co_return to_raw(directive);
        }
    }

    // The eager index query is safe for dirty sessions too: freshness
    // clause 4 serves their shard only while the buffer is byte-identical,
    // and rejects the mixed-view lookup otherwise.
    if(auto result = index_query.query_definition(path, pos, session.get()); !result.empty()) {
        co_return to_raw(result);
    }

    if(!session)
        co_return kota::outcome_error(document_not_open());

    // An index-only session never owes the compile the worker forward
    // implies, a session served under freshness clause 4 (escalated,
    // compile still in flight) already routed to the index, and a
    // quarantined session cannot reach a worker at all. What the worker
    // leg covers — include directives, which have no symbol occurrence —
    // the manifest edges answer instead, under the same content gate as
    // the links projection: manifest lines are meaningless against a
    // buffer the index never described.
    if(session->serving == ServingMode::IndexOnly || session->quarantine.blocked() ||
       index_query.open_session_shard(*session)) {
        auto it = workspace.shards.find(session->path_id);
        if(it == workspace.shards.end() || !it->second.matches_content(session->text)) {
            co_return serde_raw{"[]"};
        }
        if(auto offset = session->line_map().to_offset(pos)) {
            auto links = feature::index_document_links(session->text,
                                                       index_lang_options(*session),
                                                       index_query.include_edges(*session));
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

    auto raw = co_await forwarder.forward_query(worker::QueryKind::GoToDefinition,
                                                session,
                                                pos,
                                                {},
                                                std::move(token));
    if(raw.has_value() && raw.value().data != "[]" && raw.value().data != "null") {
        co_return std::move(raw.value());
    }

    // The forward compiled a dirty buffer: retry against the refreshed
    // projection and preamble links, but only when the compile actually
    // completed — a failed or superseded compile leaves the projection
    // non-current and the caches stale.
    if(ast.projections.current(session->path_id)) {
        if(auto retry = index_query.query_definition(path, pos, session.get()); !retry.empty()) {
            co_return to_raw(retry);
        }
        if(auto directive = resolve_directive_definition(*session, pos); !directive.empty()) {
            co_return to_raw(directive);
        }
    }
    co_return std::move(raw);
}

FeatureRouter::RawResult FeatureRouter::hover(std::shared_ptr<Session> session,
                                              const protocol::Position& position,
                                              std::optional<kota::cancellation_token> token) {
    if(!session) {
        co_return kota::outcome_error(document_not_open());
    }

    auto path = workspace.path_pool.resolve(session->path_id);
    auto index_card = [&]() -> std::optional<serde_raw> {
        if(auto info = index_query.hover_card(path, position, session.get())) {
            return to_raw(
                feature::to_protocol_hover(*info, workspace.config.hover, session->line_map()));
        }
        return std::nullopt;
    };

    switch(co_await pick_route(session, /*full_lex=*/false)) {
        case Route::Superseded: co_return serde_raw{"null"};
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
    auto gen = session->generation;
    auto raw = co_await forwarder.forward_query(worker::QueryKind::Hover,
                                                session,
                                                position,
                                                {},
                                                std::move(token));
    if(session->generation != gen) {
        co_return serde_raw{"null"};
    }
    if(raw.has_value() && raw.value().data == "null" && ast.projections.current(session->path_id)) {
        if(auto hover = resolve_preamble_hover(*session, position)) {
            co_return to_raw(*hover);
        }
        // The preamble region is the one place a null from the AST is not
        // authoritative — it is compiled into the PCH and invisible to
        // the worker (a `#define` there has no node). The index card
        // fills that gap, and only that gap: past the bound the AST saw
        // the code and declined on purpose (a lambda's auto parameter
        // must not hover, and the index would happily name it `auto:1`).
        auto projection = ast.projections.projection(session->path_id);
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

FeatureRouter::RawResult
    FeatureRouter::semantic_tokens(std::shared_ptr<Session> session,
                                   std::optional<kota::cancellation_token> token) {
    if(session) {
        IndexRowsSource source;
        switch(co_await pick_route(session, /*full_lex=*/true, &source)) {
            case Route::Superseded: co_return serde_raw{"null"};
            case Route::Index: {
                auto rows = extract_rows(*source.shard);
                auto tokens = feature::index_semantic_tokens(
                    session->text,
                    index_lang_options(*session),
                    rows.occurrences,
                    rows.decls,
                    [&](index::SymbolHash hash) { return resolve_symbol_info(hash); });
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
    }
    co_return co_await forwarder.forward_query(worker::QueryKind::SemanticTokens,
                                               session,
                                               {},
                                               {},
                                               std::move(token));
}

FeatureRouter::RawResult FeatureRouter::inlay_hints(std::shared_ptr<Session> session,
                                                    const protocol::Range& range,
                                                    std::optional<kota::cancellation_token> token) {
    // Inlay hints are Sema products the index cannot project; a session
    // the policy keeps un-compiled answers honestly empty. The compile
    // that follows an escalation pushes an inlayHint refresh, so clients
    // re-pull once real hints exist.
    if(session && session->serving == ServingMode::IndexOnly) {
        session->index_served = true;
        co_return serde_raw{"[]"};
    }
    co_return co_await forwarder.forward_query(worker::QueryKind::InlayHints,
                                               session,
                                               {},
                                               range,
                                               std::move(token));
}

FeatureRouter::RawResult
    FeatureRouter::folding_range(std::shared_ptr<Session> session,
                                 std::optional<kota::cancellation_token> token) {
    if(session) {
        IndexRowsSource source;
        switch(co_await pick_route(session, /*full_lex=*/true, &source)) {
            case Route::Superseded: co_return serde_raw{"null"};
            case Route::Index: {
                auto rows = extract_rows(*source.shard);
                auto folds = feature::index_folding_ranges(
                    session->text,
                    index_lang_options(*session),
                    rows.decls,
                    [&](index::SymbolHash hash) { return resolve_symbol_info(hash); });
                session->index_served = true;
                co_return to_raw(
                    feature::folding_ranges_to_protocol(folds,
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
    }
    co_return co_await forwarder.forward_query(worker::QueryKind::FoldingRange,
                                               session,
                                               {},
                                               {},
                                               std::move(token));
}

FeatureRouter::RawResult
    FeatureRouter::document_symbol(std::shared_ptr<Session> session,
                                   std::optional<kota::cancellation_token> token) {
    if(session) {
        for(bool waited = false;;) {
            IndexRowsSource source;
            switch(co_await pick_route(session, /*full_lex=*/false, &source)) {
                case Route::Superseded: co_return serde_raw{"null"};
                case Route::Index: {
                    auto rows = extract_rows(*source.shard);
                    auto symbols =
                        feature::index_document_symbols(rows.decls, [&](index::SymbolHash hash) {
                            return resolve_symbol_info(hash);
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
                        auto gen = session->generation;
                        co_await pump.await_attempt(session->path_id);
                        if(session->generation == gen) {
                            continue;
                        }
                        co_return serde_raw{"null"};
                    }
                    co_return serde_raw{"[]"};
                }
                case Route::Ast: break;
            }
            break;
        }
    }
    co_return co_await forwarder.forward_query(worker::QueryKind::DocumentSymbol,
                                               session,
                                               {},
                                               {},
                                               std::move(token));
}

FeatureRouter::RawResult FeatureRouter::code_action(std::shared_ptr<Session> session,
                                                    std::optional<kota::cancellation_token> token) {
    // Code actions are AST products with no index projection; a session
    // the policy keeps un-compiled answers honestly empty rather than
    // forcing the compile the policy declined.
    if(session && !ast_answerable(*session) && session->serving == ServingMode::IndexOnly) {
        co_return serde_raw{"[]"};
    }
    co_return co_await forwarder.forward_query(worker::QueryKind::CodeAction,
                                               session,
                                               {},
                                               {},
                                               std::move(token));
}

FeatureRouter::RawResult FeatureRouter::completion(std::shared_ptr<Session> session,
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
    co_await kota::yield();

    // The drain may also have replaced the Session object (a didClose,
    // with or without a reopen). Downstream generation checks snapshot
    // the dead object after its close already bumped it, so only the
    // store can tell; the request belongs to the discarded buffer.
    if(sessions.find(session->path_id) != session) {
        co_return serde_raw{"null"};
    }

    auto path_id = session->path_id;
    auto path = std::string(workspace.path_pool.resolve(path_id));

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
            auto module_names = complete_module_import(workspace.path_to_module, pctx.prefix);

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

    co_return co_await forwarder.forward_completion(position, std::move(session), std::move(token));
}

FeatureRouter::RawResult
    FeatureRouter::signature_help(std::shared_ptr<Session> session,
                                  const protocol::Position& position,
                                  std::optional<kota::cancellation_token> token) {
    auto pause = pump.scoped_pause();
    ast.escalate(*session);
    co_return co_await forwarder.forward_signature_help(position, session, std::move(token));
}

FeatureRouter::RawResult FeatureRouter::formatting(std::shared_ptr<Session> session,
                                                   std::optional<kota::cancellation_token> token) {
    auto pause = pump.scoped_pause();
    co_return co_await forwarder.forward_format(session, {}, std::move(token));
}

FeatureRouter::RawResult
    FeatureRouter::range_formatting(std::shared_ptr<Session> session,
                                    const protocol::Range& range,
                                    std::optional<kota::cancellation_token> token) {
    auto pause = pump.scoped_pause();
    co_return co_await forwarder.forward_format(session, range, std::move(token));
}

FeatureRouter::RawResult FeatureRouter::references(std::shared_ptr<Session> session,
                                                   llvm::StringRef path,
                                                   const protocol::Position& position,
                                                   bool include_declaration) {
    if(session && !co_await nav_gate(session)) {
        co_return serde_raw{"null"};
    }

    co_return to_raw(
        index_query.query_references(path, position, include_declaration, session.get()));
}

FeatureRouter::RawResult FeatureRouter::declaration(std::shared_ptr<Session> session,
                                                    llvm::StringRef path,
                                                    const protocol::Position& position) {
    if(session && !co_await nav_gate(session)) {
        co_return serde_raw{"null"};
    }

    co_return to_raw(index_query.query_declaration(path, position, session.get()));
}

FeatureRouter::RawResult FeatureRouter::type_definition(std::shared_ptr<Session> session,
                                                        llvm::StringRef path,
                                                        const protocol::Position& position) {
    if(session && !co_await nav_gate(session)) {
        co_return serde_raw{"null"};
    }

    co_return to_raw(index_query.query_symbol_targets(path,
                                                      position,
                                                      RelationKind::TypeDefinition,
                                                      session.get()));
}

FeatureRouter::RawResult FeatureRouter::implementation(std::shared_ptr<Session> session,
                                                       llvm::StringRef path,
                                                       const protocol::Position& position) {
    if(session && !co_await nav_gate(session)) {
        co_return serde_raw{"null"};
    }

    co_return to_raw(index_query.query_implementation(path, position, session.get()));
}

FeatureRouter::RawResult FeatureRouter::call_hierarchy_prepare(std::shared_ptr<Session> session,
                                                               const std::string& uri,
                                                               llvm::StringRef path,
                                                               const protocol::Position& position) {
    if(session && !co_await nav_gate(session)) {
        co_return serde_raw{"null"};
    }

    auto info = index_query.lookup_symbol(uri, path, position, session.get());
    if(!info)
        co_return serde_raw{"null"};
    if(!(info->kind == SymbolKind::Function || info->kind == SymbolKind::Method ||
         info->kind == SymbolKind::Operator))
        co_return serde_raw{"null"};

    // The item stands for the symbol, not the cursor: anchor it at the
    // symbol's canonical site so expanding from a use renders the same
    // root as expanding from the declaration.
    if(auto canonical = index_query.resolve_symbol(info->hash))
        info = canonical;

    std::vector<protocol::CallHierarchyItem> items;
    items.push_back(IndexQuery::build_call_hierarchy_item(*info));
    co_return to_raw(items);
}

FeatureRouter::RawResult
    FeatureRouter::call_hierarchy_incoming(std::shared_ptr<Session> session,
                                           llvm::StringRef path,
                                           const protocol::CallHierarchyItem& item) {
    // No compile gate here: expansion resolves the previously prepared
    // item through its stored symbol handle (item.data, with the recorded
    // range as fallback), not the current cursor — the buffer's present
    // compile state is irrelevant, and gating would blank expansions the
    // moment the user edits the file again.

    auto info =
        index_query.resolve_hierarchy_item(item.uri, path, item.range, item.data, session.get());
    if(!info)
        co_return kota::outcome_error(item_not_resolved("call hierarchy"));
    auto results = index_query.find_incoming_calls(info->hash);
    co_return to_raw(results);
}

FeatureRouter::RawResult
    FeatureRouter::call_hierarchy_outgoing(std::shared_ptr<Session> session,
                                           llvm::StringRef path,
                                           const protocol::CallHierarchyItem& item) {
    // No compile gate here: expansion resolves the previously prepared
    // item through its stored symbol handle (item.data, with the recorded
    // range as fallback), not the current cursor — the buffer's present
    // compile state is irrelevant, and gating would blank expansions the
    // moment the user edits the file again.

    auto info =
        index_query.resolve_hierarchy_item(item.uri, path, item.range, item.data, session.get());
    if(!info)
        co_return kota::outcome_error(item_not_resolved("call hierarchy"));
    auto results = index_query.find_outgoing_calls(info->hash);
    co_return to_raw(results);
}

FeatureRouter::RawResult FeatureRouter::type_hierarchy_prepare(std::shared_ptr<Session> session,
                                                               const std::string& uri,
                                                               llvm::StringRef path,
                                                               const protocol::Position& position) {
    if(session && !co_await nav_gate(session)) {
        co_return serde_raw{"null"};
    }

    auto info = index_query.lookup_symbol(uri, path, position, session.get());
    if(!info)
        co_return serde_raw{"null"};
    if(!(info->kind == SymbolKind::Class || info->kind == SymbolKind::Struct ||
         info->kind == SymbolKind::Enum || info->kind == SymbolKind::Union))
        co_return serde_raw{"null"};

    if(auto canonical = index_query.resolve_symbol(info->hash))
        info = canonical;

    std::vector<protocol::TypeHierarchyItem> items;
    items.push_back(IndexQuery::build_type_hierarchy_item(*info));
    co_return to_raw(items);
}

FeatureRouter::RawResult
    FeatureRouter::type_hierarchy_supertypes(std::shared_ptr<Session> session,
                                             llvm::StringRef path,
                                             const protocol::TypeHierarchyItem& item) {
    // No compile gate here: expansion resolves the previously prepared
    // item through its stored symbol handle (item.data, with the recorded
    // range as fallback), not the current cursor — the buffer's present
    // compile state is irrelevant, and gating would blank expansions the
    // moment the user edits the file again.

    auto info =
        index_query.resolve_hierarchy_item(item.uri, path, item.range, item.data, session.get());
    if(!info)
        co_return kota::outcome_error(item_not_resolved("type hierarchy"));
    auto results = index_query.find_supertypes(info->hash);
    co_return to_raw(results);
}

FeatureRouter::RawResult
    FeatureRouter::type_hierarchy_subtypes(std::shared_ptr<Session> session,
                                           llvm::StringRef path,
                                           const protocol::TypeHierarchyItem& item) {
    // No compile gate here: expansion resolves the previously prepared
    // item through its stored symbol handle (item.data, with the recorded
    // range as fallback), not the current cursor — the buffer's present
    // compile state is irrelevant, and gating would blank expansions the
    // moment the user edits the file again.

    auto info =
        index_query.resolve_hierarchy_item(item.uri, path, item.range, item.data, session.get());
    if(!info)
        co_return kota::outcome_error(item_not_resolved("type hierarchy"));
    auto results = index_query.find_subtypes(info->hash);
    co_return to_raw(results);
}

FeatureRouter::RawResult FeatureRouter::workspace_symbol(llvm::StringRef query) {
    co_return to_raw(index_query.search_symbols(query));
}

}  // namespace clice

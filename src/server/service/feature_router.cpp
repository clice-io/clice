#include "server/service/feature_router.h"

#include <algorithm>
#include <format>
#include <iterator>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "command/search_config.h"
#include "semantic/relation_kind.h"
#include "semantic/symbol_kind.h"
#include "server/compiler/compiler.h"
#include "server/compiler/context_resolver.h"
#include "server/compiler/indexer.h"
#include "server/protocol/serialize.h"
#include "server/protocol/worker.h"
#include "syntax/completion.h"
#include "syntax/include_resolver.h"

#include "kota/codec/json/json.h"

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

std::vector<feature::DocumentLink> FeatureRouter::find_preamble_links(const Session& session) {
    if(!session.pch_key)
        return {};
    auto it = workspace.pch_cache.find(*session.pch_key);
    if(it == workspace.pch_cache.end())
        return {};
    auto& state = it->second.load_state();
    if(!state)
        return {};
    // Link offsets are buffer coordinates as of the PCH build; serve them
    // only while the buffer still starts with that exact preamble text
    // (a deferred rebuild mid-edit keeps an old blob for a moved buffer).
    if(!llvm::StringRef(session.text).starts_with(state->preamble_content()))
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
        if(link.range.contains(*offset)) {
            locations.push_back(protocol::Location{
                .uri = feature::to_uri(link.target),
                .range = protocol::Range{},
            });
            break;
        }
    }
    return locations;
}

kota::task<std::vector<protocol::DocumentLink>, kota::ipc::Error>
    FeatureRouter::document_links(std::shared_ptr<Session> session,
                                  std::optional<kota::cancellation_token> token) {
    auto result = co_await compiler.forward_document_links(session, std::move(token));
    if(!result.has_value())
        co_return kota::outcome_error(std::move(result.error()));

    // The preamble is compiled into the PCH, so the worker's AST only
    // covers the rest of the file — merge the preamble's links in front.
    // Links carry byte offsets; this reply edge converts them.
    std::vector<protocol::DocumentLink> links;
    auto map = session->line_map();
    auto append = [&](const feature::DocumentLink& link) {
        auto range = map.to_range(link.range.begin, link.range.end);
        if(!range)
            return;
        protocol::DocumentLink out{.range = *range};
        out.target = link.target;
        links.push_back(std::move(out));
    };
    std::ranges::for_each(find_preamble_links(*session), append);
    std::ranges::for_each(result.value(), append);
    co_return links;
}

kota::task<kota::codec::RawValue, kota::ipc::Error>
    FeatureRouter::definition(std::shared_ptr<Session> session,
                              llvm::StringRef path,
                              const protocol::Position& pos,
                              std::optional<kota::cancellation_token> token) {
    // Same posture as every AST-backed request: the session's file index
    // is produced by the very compile awaited here, so once this settles
    // the index describes the buffer. A failed or superseded compile
    // (buffer changed while awaiting) yields null rather than a lookup
    // against positions the buffer no longer has.
    if(session) {
        auto gen = session->generation;
        if(!co_await compiler.ensure_compiled(session) || session->generation != gen) {
            co_return serde_raw{"null"};
        }
    }

    // Preamble include lines first: they have no symbol occurrence in
    // the index and are invisible to the worker's AST. A session dirty even
    // after the awaited compile means the world was re-dirtied mid-flight
    // (dirty_epoch moved, so the settle did not clear the flag): the cached
    // links may describe a pre-edit preamble — skip, and let the index and
    // worker paths below answer.
    if(session && !session->ast_dirty) {
        if(auto directive = resolve_directive_definition(*session, pos); !directive.empty()) {
            co_return to_raw(directive);
        }
    }

    // Dirty sessions also skip the eager index query: resolve_cursor
    // would fall back to the stale merged shard and could return a
    // non-empty hit for pre-edit content, bypassing the compile below.
    if(!session || !session->ast_dirty) {
        auto result =
            index_query.query_relations(path, pos, RelationKind::Definition, session.get());
        if(!result.empty()) {
            co_return to_raw(result);
        }
    }

    if(!session)
        co_return kota::outcome_error(document_not_open());
    auto raw = co_await compiler.forward_query(worker::QueryKind::GoToDefinition,
                                               session,
                                               pos,
                                               {},
                                               std::move(token));
    if(raw.has_value() && raw.value().data != "[]" && raw.value().data != "null") {
        co_return std::move(raw.value());
    }

    // The forward compiled a dirty buffer: retry against the refreshed
    // session index and preamble links, but only when the compile
    // actually completed — a failed or superseded compile leaves
    // ast_dirty set and the caches stale.
    if(!session->ast_dirty) {
        if(auto retry =
               index_query.query_relations(path, pos, RelationKind::Definition, session.get());
           !retry.empty()) {
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
    co_return co_await compiler.forward_query(worker::QueryKind::Hover,
                                              session,
                                              position,
                                              {},
                                              std::move(token));
}

FeatureRouter::RawResult
    FeatureRouter::semantic_tokens(std::shared_ptr<Session> session,
                                   std::optional<kota::cancellation_token> token) {
    co_return co_await compiler.forward_query(worker::QueryKind::SemanticTokens,
                                              session,
                                              {},
                                              {},
                                              std::move(token));
}

FeatureRouter::RawResult FeatureRouter::inlay_hints(std::shared_ptr<Session> session,
                                                    const protocol::Range& range,
                                                    std::optional<kota::cancellation_token> token) {
    co_return co_await compiler.forward_query(worker::QueryKind::InlayHints,
                                              session,
                                              {},
                                              range,
                                              std::move(token));
}

FeatureRouter::RawResult
    FeatureRouter::folding_range(std::shared_ptr<Session> session,
                                 std::optional<kota::cancellation_token> token) {
    co_return co_await compiler.forward_query(worker::QueryKind::FoldingRange,
                                              session,
                                              {},
                                              {},
                                              std::move(token));
}

FeatureRouter::RawResult
    FeatureRouter::document_symbol(std::shared_ptr<Session> session,
                                   std::optional<kota::cancellation_token> token) {
    co_return co_await compiler.forward_query(worker::QueryKind::DocumentSymbol,
                                              session,
                                              {},
                                              {},
                                              std::move(token));
}

FeatureRouter::RawResult FeatureRouter::code_action(std::shared_ptr<Session> session,
                                                    std::optional<kota::cancellation_token> token) {
    co_return co_await compiler.forward_query(worker::QueryKind::CodeAction,
                                              session,
                                              {},
                                              {},
                                              std::move(token));
}

FeatureRouter::RawResult FeatureRouter::completion(std::shared_ptr<Session> session,
                                                   const protocol::Position& position,
                                                   std::optional<kota::cancellation_token> token) {
    auto pause = indexer.scoped_pause();

    // This handler is resumed eagerly, so a $/cancelRequest or didChange
    // sitting in the pipe (rapid-fire completions cancel and re-issue as
    // the user types) has not been read yet. Yield once BEFORE reading any
    // buffer state: the loop drains the pipe — a fired token tears this
    // frame down here, and an edit lands before the offset and completion
    // context are computed, so the synchronous include scan below never
    // serves candidates or ranges for a buffer that no longer exists.
    co_await kota::yield();

    auto path_id = session->path_id;
    auto path = std::string(workspace.path_pool.resolve(path_id));

    auto map = session->line_map();
    auto offset = map.to_offset(position);
    if(offset) {
        auto pctx = detect_completion_context(session->text, *offset);
        if(pctx.kind == CompletionContext::IncludeQuoted ||
           pctx.kind == CompletionContext::IncludeAngled) {
            std::string directory;
            std::vector<std::string> arguments;
            contexts.resolve_command(path, directory, arguments);

            std::vector<const char*> args_ptrs;
            args_ptrs.reserve(arguments.size());
            for(auto& arg: arguments)
                args_ptrs.push_back(arg.c_str());

            auto search_config = extract_search_config(args_ptrs, directory);
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
            auto json = kota::codec::json::to_json<kota::ipc::lsp_config>(items);
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
            auto json = kota::codec::json::to_json<kota::ipc::lsp_config>(items);
            co_return serde_raw{json ? std::move(*json) : "[]"};
        }
    }

    co_return co_await compiler.forward_build(worker::BuildKind::Completion,
                                              position,
                                              std::move(session),
                                              std::move(token));
}

FeatureRouter::RawResult
    FeatureRouter::signature_help(std::shared_ptr<Session> session,
                                  const protocol::Position& position,
                                  std::optional<kota::cancellation_token> token) {
    auto pause = indexer.scoped_pause();
    co_return co_await compiler.forward_build(worker::BuildKind::SignatureHelp,
                                              position,
                                              session,
                                              std::move(token));
}

FeatureRouter::RawResult FeatureRouter::formatting(std::shared_ptr<Session> session,
                                                   std::optional<kota::cancellation_token> token) {
    auto pause = indexer.scoped_pause();
    co_return co_await compiler.forward_format(session, {}, std::move(token));
}

FeatureRouter::RawResult
    FeatureRouter::range_formatting(std::shared_ptr<Session> session,
                                    const protocol::Range& range,
                                    std::optional<kota::cancellation_token> token) {
    auto pause = indexer.scoped_pause();
    co_return co_await compiler.forward_format(session, range, std::move(token));
}

FeatureRouter::RawResult FeatureRouter::references(std::shared_ptr<Session> session,
                                                   llvm::StringRef path,
                                                   const protocol::Position& position,
                                                   bool include_declaration) {
    // Same posture as every AST-backed request: the session's file index
    // is produced by the very compile awaited here, so once this settles
    // the index describes the buffer. A failed or superseded compile
    // (buffer changed while awaiting) yields null rather than a lookup
    // against positions the buffer no longer has.
    if(session) {
        auto gen = session->generation;
        if(!co_await compiler.ensure_compiled(session) || session->generation != gen) {
            co_return serde_raw{"null"};
        }
    }

    auto locations =
        index_query.query_relations(path, position, RelationKind::Reference, session.get());

    if(include_declaration) {
        for(auto kind: {RelationKind::Declaration, RelationKind::Definition}) {
            auto extra = index_query.query_relations(path, position, kind, session.get());
            locations.insert(locations.end(),
                             std::make_move_iterator(extra.begin()),
                             std::make_move_iterator(extra.end()));
        }
    }

    co_return to_raw(locations);
}

/// Declarations plus the definition: symbols defined inline have no
/// separate Declaration relation, and navigating to the definition is
/// what every client expects in that case.
FeatureRouter::RawResult FeatureRouter::declaration(std::shared_ptr<Session> session,
                                                    llvm::StringRef path,
                                                    const protocol::Position& position) {
    // Same posture as every AST-backed request: the session's file index
    // is produced by the very compile awaited here, so once this settles
    // the index describes the buffer. A failed or superseded compile
    // (buffer changed while awaiting) yields null rather than a lookup
    // against positions the buffer no longer has.
    if(session) {
        auto gen = session->generation;
        if(!co_await compiler.ensure_compiled(session) || session->generation != gen) {
            co_return serde_raw{"null"};
        }
    }

    auto locations =
        index_query.query_relations(path, position, RelationKind::Declaration, session.get());
    auto defs =
        index_query.query_relations(path, position, RelationKind::Definition, session.get());
    locations.insert(locations.end(),
                     std::make_move_iterator(defs.begin()),
                     std::make_move_iterator(defs.end()));
    co_return to_raw(locations);
}

FeatureRouter::RawResult FeatureRouter::type_definition(std::shared_ptr<Session> session,
                                                        llvm::StringRef path,
                                                        const protocol::Position& position) {
    // Same posture as every AST-backed request: the session's file index
    // is produced by the very compile awaited here, so once this settles
    // the index describes the buffer. A failed or superseded compile
    // (buffer changed while awaiting) yields null rather than a lookup
    // against positions the buffer no longer has.
    if(session) {
        auto gen = session->generation;
        if(!co_await compiler.ensure_compiled(session) || session->generation != gen) {
            co_return serde_raw{"null"};
        }
    }

    co_return to_raw(index_query.query_symbol_targets(path,
                                                      position,
                                                      RelationKind::TypeDefinition,
                                                      session.get()));
}

FeatureRouter::RawResult FeatureRouter::implementation(std::shared_ptr<Session> session,
                                                       llvm::StringRef path,
                                                       const protocol::Position& position) {
    // Same posture as every AST-backed request: the session's file index
    // is produced by the very compile awaited here, so once this settles
    // the index describes the buffer. A failed or superseded compile
    // (buffer changed while awaiting) yields null rather than a lookup
    // against positions the buffer no longer has.
    if(session) {
        auto gen = session->generation;
        if(!co_await compiler.ensure_compiled(session) || session->generation != gen) {
            co_return serde_raw{"null"};
        }
    }

    co_return to_raw(index_query.query_symbol_targets(path,
                                                      position,
                                                      RelationKind::Implementation,
                                                      session.get()));
}

FeatureRouter::RawResult FeatureRouter::call_hierarchy_prepare(std::shared_ptr<Session> session,
                                                               const std::string& uri,
                                                               llvm::StringRef path,
                                                               const protocol::Position& position) {
    // Same posture as every AST-backed request: the session's file index
    // is produced by the very compile awaited here, so once this settles
    // the index describes the buffer. A failed or superseded compile
    // (buffer changed while awaiting) yields null rather than a lookup
    // against positions the buffer no longer has.
    if(session) {
        auto gen = session->generation;
        if(!co_await compiler.ensure_compiled(session) || session->generation != gen) {
            co_return serde_raw{"null"};
        }
    }

    auto info = index_query.lookup_symbol(uri, path, position, session.get());
    if(!info)
        co_return serde_raw{"null"};
    if(!(info->kind == SymbolKind::Function || info->kind == SymbolKind::Method))
        co_return serde_raw{"null"};

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
    // Same posture as every AST-backed request: the session's file index
    // is produced by the very compile awaited here, so once this settles
    // the index describes the buffer. A failed or superseded compile
    // (buffer changed while awaiting) yields null rather than a lookup
    // against positions the buffer no longer has.
    if(session) {
        auto gen = session->generation;
        if(!co_await compiler.ensure_compiled(session) || session->generation != gen) {
            co_return serde_raw{"null"};
        }
    }

    auto info = index_query.lookup_symbol(uri, path, position, session.get());
    if(!info)
        co_return serde_raw{"null"};
    if(!(info->kind == SymbolKind::Class || info->kind == SymbolKind::Struct ||
         info->kind == SymbolKind::Enum || info->kind == SymbolKind::Union))
        co_return serde_raw{"null"};

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

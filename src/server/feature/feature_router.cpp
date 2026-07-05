#include "server/feature/feature_router.h"

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
#include "server/context/context_resolver.h"
#include "server/index/background_indexer.h"
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

const std::vector<feature::DocumentLink>*
    FeatureRouter::find_preamble_links(const Session& session) {
    if(!session.pch_ref)
        return nullptr;
    auto it = workspace.pch_cache.find(session.pch_ref->key);
    if(it == workspace.pch_cache.end() || it->second.preamble_links.empty())
        return nullptr;
    return &it->second.preamble_links;
}

std::vector<protocol::Location>
    FeatureRouter::resolve_directive_definition(Session& session,
                                                const protocol::Position& position) {
    std::vector<protocol::Location> locations;

    // Preamble include lines: compiled into the PCH, invisible to the
    // worker's AST — the PCH's cached links carry the targets.
    if(auto* links = find_preamble_links(session)) {
        for(auto& link: *links) {
            if(link.range.start.line != position.line)
                continue;
            if(position.character < link.range.start.character ||
               position.character > link.range.end.character)
                continue;
            locations.push_back(protocol::Location{
                .uri = feature::to_uri(link.target),
                .range = protocol::Range{},
            });
            return locations;
        }
    }

    return locations;
}

kota::task<std::vector<protocol::DocumentLink>, kota::ipc::Error>
    FeatureRouter::document_links(std::shared_ptr<Session> session) {
    auto result = co_await compiler.forward_document_links(session);
    if(!result.has_value())
        co_return kota::outcome_error(std::move(result.error()));

    // The preamble is compiled into the PCH, so the worker's AST only
    // covers the rest of the file — merge the preamble's links in front.
    std::vector<protocol::DocumentLink> links;
    auto append = [&](const feature::DocumentLink& link) {
        protocol::DocumentLink out{.range = link.range};
        out.target = link.target;
        links.push_back(std::move(out));
    };
    // Skipped while dirty: a failed or superseded compile leaves
    // the cached links describing the pre-edit preamble.
    if(!session->ast_dirty) {
        if(auto* pch_links = find_preamble_links(*session)) {
            std::ranges::for_each(*pch_links, append);
        }
    }
    std::ranges::for_each(result.value(), append);
    co_return links;
}

kota::task<kota::codec::RawValue, kota::ipc::Error>
    FeatureRouter::definition(std::shared_ptr<Session> session,
                              llvm::StringRef path,
                              const protocol::Position& pos) {
    // Preamble include lines first: they have no symbol occurrence in
    // the index and are invisible to the worker's AST. Dirty sessions
    // skip this — the cached links may describe the pre-edit preamble —
    // and retry below once the worker compile refreshed the PCH.
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
    auto raw = co_await compiler.forward_query(worker::QueryKind::GoToDefinition, session, pos);
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
                                              const protocol::Position& position) {
    co_return co_await compiler.forward_query(worker::QueryKind::Hover, session, position);
}

FeatureRouter::RawResult FeatureRouter::semantic_tokens(std::shared_ptr<Session> session) {
    co_return co_await compiler.forward_query(worker::QueryKind::SemanticTokens, session);
}

FeatureRouter::RawResult FeatureRouter::inlay_hints(std::shared_ptr<Session> session,
                                                    const protocol::Range& range) {
    co_return co_await compiler.forward_query(worker::QueryKind::InlayHints, session, {}, range);
}

FeatureRouter::RawResult FeatureRouter::folding_range(std::shared_ptr<Session> session) {
    co_return co_await compiler.forward_query(worker::QueryKind::FoldingRange, session);
}

FeatureRouter::RawResult FeatureRouter::document_symbol(std::shared_ptr<Session> session) {
    co_return co_await compiler.forward_query(worker::QueryKind::DocumentSymbol, session);
}

FeatureRouter::RawResult FeatureRouter::code_action(std::shared_ptr<Session> session) {
    co_return co_await compiler.forward_query(worker::QueryKind::CodeAction, session);
}

FeatureRouter::RawResult FeatureRouter::completion(std::shared_ptr<Session> session,
                                                   const protocol::Position& position) {
    auto pause = background_indexer.scoped_pause();

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
                                              std::move(session));
}

FeatureRouter::RawResult FeatureRouter::signature_help(std::shared_ptr<Session> session,
                                                       const protocol::Position& position) {
    auto pause = background_indexer.scoped_pause();
    co_return co_await compiler.forward_build(worker::BuildKind::SignatureHelp, position, session);
}

FeatureRouter::RawResult FeatureRouter::formatting(std::shared_ptr<Session> session) {
    auto pause = background_indexer.scoped_pause();
    co_return co_await compiler.forward_format(session);
}

FeatureRouter::RawResult FeatureRouter::range_formatting(std::shared_ptr<Session> session,
                                                         const protocol::Range& range) {
    auto pause = background_indexer.scoped_pause();
    co_return co_await compiler.forward_format(session, range);
}

FeatureRouter::RawResult FeatureRouter::references(std::shared_ptr<Session> session,
                                                   llvm::StringRef path,
                                                   const protocol::Position& position,
                                                   bool include_declaration) {
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
    co_return to_raw(index_query.query_symbol_targets(path,
                                                      position,
                                                      RelationKind::TypeDefinition,
                                                      session.get()));
}

FeatureRouter::RawResult FeatureRouter::implementation(std::shared_ptr<Session> session,
                                                       llvm::StringRef path,
                                                       const protocol::Position& position) {
    co_return to_raw(index_query.query_symbol_targets(path,
                                                      position,
                                                      RelationKind::Implementation,
                                                      session.get()));
}

FeatureRouter::RawResult FeatureRouter::call_hierarchy_prepare(std::shared_ptr<Session> session,
                                                               const std::string& uri,
                                                               llvm::StringRef path,
                                                               const protocol::Position& position) {
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

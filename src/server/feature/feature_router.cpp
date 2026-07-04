#include "server/feature/feature_router.h"

#include <algorithm>
#include <string>
#include <utility>

#include "semantic/relation_kind.h"
#include "server/compiler/compiler.h"
#include "server/protocol/serialize.h"
#include "server/protocol/worker.h"

namespace clice {

/// Error response for feature requests on files with no open session.
static kota::ipc::Error document_not_open() {
    return kota::ipc::Error{kota::ipc::protocol::ErrorCode::InvalidParams, "Document not open"};
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

}  // namespace clice

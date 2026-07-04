#pragma once

#include <memory>
#include <vector>

#include "feature/feature.h"
#include "server/index/query.h"
#include "server/session/session.h"
#include "server/workspace/workspace.h"

#include "kota/async/async.h"
#include "kota/codec/json/json.h"
#include "kota/ipc/codec/json.h"
#include "kota/ipc/lsp/protocol.h"

namespace clice {

class Compiler;

namespace protocol = kota::ipc::protocol;

/// Routing layer that assembles feature results from their providers.
///
/// A feature answer can come from three sources:
///   - the live AST: forwarded to the worker that holds the file's AST;
///   - build-companion caches: products derived while building the PCH
///     (e.g. the preamble's document links), cached master-side because the
///     worker's AST is built on top of the PCH and cannot see the preamble
///     region;
///   - the index: disk-derived shards queried through IndexQuery.
///
/// Features fall into two shapes. Whole-document results (document links,
/// semantic tokens, folding ranges, ...) have a payload determined entirely
/// by the document content, so they could in the future be served from a
/// content-addressed result cache. Position-parameterized queries
/// (definition, hover, completion) take a cursor and cannot be keyed that
/// way; for these the index could in the future grow into a complete
/// provider, serving files that are not open at all.
///
/// Discipline: any feature whose answer is assembled from more than one
/// source belongs here. Transports (LSP/agentic handlers) only translate
/// between the wire protocol and these methods — they never merge, retry,
/// or gate results themselves.
class FeatureRouter {
public:
    FeatureRouter(Compiler& compiler, IndexQuery& index_query, Workspace& workspace) :
        compiler(compiler), index_query(index_query), workspace(workspace) {}

    /// Full document-link result for a session: the worker's main-file links
    /// merged behind the PCH's cached preamble links.
    kota::task<std::vector<protocol::DocumentLink>, kota::ipc::Error>
        document_links(std::shared_ptr<Session> session);

    /// Go-to-definition, assembled across all providers: preamble directive
    /// targets, the index, and the worker's AST, with an index/directive
    /// retry after the forward's compile refreshes a dirty session.
    /// @param session may be null (document not open).
    kota::task<kota::codec::RawValue, kota::ipc::Error> definition(std::shared_ptr<Session> session,
                                                                   llvm::StringRef path,
                                                                   const protocol::Position& pos);

private:
    /// The preamble include links of a session's active PCH, or nullptr.
    const std::vector<feature::DocumentLink>* find_preamble_links(const Session& session);

    /// Resolve go-to-definition on a preamble include line that the worker
    /// AST cannot see: the include is compiled into the PCH, so the target
    /// is answered from the PCH's cached preamble links. Module names go
    /// through the ordinary index pipeline, not this path.
    std::vector<protocol::Location>
        resolve_directive_definition(Session& session, const protocol::Position& position);

    Compiler& compiler;
    IndexQuery& index_query;
    Workspace& workspace;
};

}  // namespace clice

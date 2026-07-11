#pragma once

#include <memory>
#include <vector>

#include "feature/feature.h"
#include "server/service/query.h"
#include "server/state/session.h"
#include "server/state/workspace.h"

#include "kota/async/async.h"
#include "kota/codec/json/json.h"
#include "kota/ipc/codec/json.h"
#include "kota/ipc/lsp/protocol.h"

namespace clice {

class Compiler;
class ContextResolver;
class Indexer;

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
    FeatureRouter(Compiler& compiler,
                  IndexQuery& index_query,
                  Workspace& workspace,
                  ContextResolver& contexts,
                  Indexer& indexer) :
        compiler(compiler), index_query(index_query), workspace(workspace), contexts(contexts),
        indexer(indexer) {}

    using RawResult = kota::task<kota::codec::RawValue, kota::ipc::Error>;

    /// Full document-link result for a session: the worker's main-file links
    /// merged behind the PCH's cached preamble links.
    kota::task<std::vector<protocol::DocumentLink>, kota::ipc::Error>
        document_links(std::shared_ptr<Session> session);

    /// Go-to-definition, assembled across all providers: preamble directive
    /// targets, the index, and the worker's AST, with an index/directive
    /// retry after the forward's compile refreshes a dirty session.
    /// @param session may be null (document not open).
    RawResult definition(std::shared_ptr<Session> session,
                         llvm::StringRef path,
                         const protocol::Position& pos);

    /// Single-source features routed through the router as a matter of
    /// discipline, not necessity. Each currently forwards to exactly one
    /// provider (the worker's AST, a stateless build, or the index), so most
    /// are one-line delegations. They live here as reserved tenants: the
    /// router is where a feature's answer is assembled once it draws on more
    /// than one source, and these are the designated hooks for a future
    /// read-only provider strategy (e.g. serving closed files from the index).
    /// They are NOT dead code to be inlined back into the transports.
    RawResult hover(std::shared_ptr<Session> session, const protocol::Position& position);
    RawResult semantic_tokens(std::shared_ptr<Session> session);
    RawResult inlay_hints(std::shared_ptr<Session> session, const protocol::Range& range);
    RawResult folding_range(std::shared_ptr<Session> session);
    RawResult document_symbol(std::shared_ptr<Session> session);
    RawResult code_action(std::shared_ptr<Session> session);

    /// Code completion. Serves preamble contexts (include/import) locally from
    /// the include graph and module map; delegates ordinary code completion to
    /// a stateless worker. Pauses background indexing for the request's span.
    RawResult completion(std::shared_ptr<Session> session, const protocol::Position& position);

    /// Signature help, forwarded to a stateless build. Pauses background
    /// indexing for the request's span.
    RawResult signature_help(std::shared_ptr<Session> session, const protocol::Position& position);

    /// Whole-document and range formatting, forwarded to a stateless worker.
    /// Pause background indexing for the request's span.
    RawResult formatting(std::shared_ptr<Session> session);
    RawResult range_formatting(std::shared_ptr<Session> session, const protocol::Range& range);

    /// Index navigation queries. Closed documents are fully serveable from the
    /// index and an empty result is a real answer (returned as []). @param
    /// session may be null (document not open) — the index is queried anyway.
    RawResult references(std::shared_ptr<Session> session,
                         llvm::StringRef path,
                         const protocol::Position& position,
                         bool include_declaration);
    RawResult declaration(std::shared_ptr<Session> session,
                          llvm::StringRef path,
                          const protocol::Position& position);
    RawResult type_definition(std::shared_ptr<Session> session,
                              llvm::StringRef path,
                              const protocol::Position& position);
    RawResult implementation(std::shared_ptr<Session> session,
                             llvm::StringRef path,
                             const protocol::Position& position);

    RawResult call_hierarchy_prepare(std::shared_ptr<Session> session,
                                     const std::string& uri,
                                     llvm::StringRef path,
                                     const protocol::Position& position);
    RawResult call_hierarchy_incoming(std::shared_ptr<Session> session,
                                      llvm::StringRef path,
                                      const protocol::CallHierarchyItem& item);
    RawResult call_hierarchy_outgoing(std::shared_ptr<Session> session,
                                      llvm::StringRef path,
                                      const protocol::CallHierarchyItem& item);

    RawResult type_hierarchy_prepare(std::shared_ptr<Session> session,
                                     const std::string& uri,
                                     llvm::StringRef path,
                                     const protocol::Position& position);
    RawResult type_hierarchy_supertypes(std::shared_ptr<Session> session,
                                        llvm::StringRef path,
                                        const protocol::TypeHierarchyItem& item);
    RawResult type_hierarchy_subtypes(std::shared_ptr<Session> session,
                                      llvm::StringRef path,
                                      const protocol::TypeHierarchyItem& item);

    RawResult workspace_symbol(llvm::StringRef query);

private:
    /// The preamble include links of a session's active PCH; empty when
    /// there is no PCH or its preamble no longer matches the buffer.
    std::vector<feature::DocumentLink> find_preamble_links(const Session& session);

    /// Resolve go-to-definition on a preamble include line that the worker
    /// AST cannot see: the include is compiled into the PCH, so the target
    /// is answered from the PCH's cached preamble links. Module names go
    /// through the ordinary index pipeline, not this path.
    std::vector<protocol::Location>
        resolve_directive_definition(Session& session, const protocol::Position& position);

    Compiler& compiler;
    IndexQuery& index_query;
    Workspace& workspace;
    ContextResolver& contexts;
    Indexer& indexer;
};

}  // namespace clice

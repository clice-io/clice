#pragma once

#include <memory>
#include <optional>
#include <vector>

#include "feature/feature.h"
#include "sched/workspace.h"
#include "server/service/dispatcher.h"
#include "server/service/query.h"
#include "server/state/session.h"
#include "server/state/session_store.h"

#include "kota/async/async.h"
#include "kota/codec/json/json.h"
#include "kota/ipc/codec/json.h"
#include "kota/ipc/lsp/protocol.h"

namespace clice {

class ASTFamily;
class ContextResolver;
class IndexPump;

namespace protocol = kota::ipc::protocol;

/// The server's language features, each assembled from its providers.
///
/// A feature answer can come from three sources:
///   - the live AST: dispatched to the worker that holds the file's AST;
///   - build-companion caches: products derived while building the PCH
///     (e.g. the preamble's document links), cached master-side because the
///     worker's AST is built on top of the PCH and cannot see the preamble
///     region;
///   - the index: every index source, read through IndexQuery.
///
/// Routing is derived per request from readiness, never from a mode
/// flag: a current AST answers as today; otherwise an index source that
/// is byte-identical to the buffer answers immediately (whole-document
/// features through the feature::index_* projections, cursor queries
/// through IndexQuery's freshness clauses); otherwise the request awaits
/// the compile the policy owes, or answers honestly empty when it owes
/// none (ServingMode::IndexOnly).
///
/// Every entry takes its Ticket before its first suspension, drains the
/// transport pipe, and answers ContentModified once the ticket goes
/// stale: the client keeps what it has and re-pulls. The one exception is
/// completion, which deliberately serves the drained buffer (see there).
///
/// Discipline: any feature whose answer is assembled from more than one
/// source belongs here. Transports (LSP/agentic handlers) only translate
/// between the wire protocol and these methods — they never merge, retry,
/// or gate results themselves.
class Features {
public:
    Features(ASTFamily& ast,
             Dispatcher& dispatcher,
             IndexQuery& query,
             Workspace& workspace,
             ContextResolver& contexts,
             IndexPump& pump,
             SessionStore& sessions) :
        ast(ast), dispatcher(dispatcher), query(query), workspace(workspace), contexts(contexts),
        pump(pump), sessions(sessions) {}

    using RawResult = Dispatcher::RawResult;

    /// Full document-link result for a session: the worker's main-file links
    /// merged behind the PCH's cached preamble links.
    kota::task<std::vector<protocol::DocumentLink>, kota::ipc::Error>
        document_links(std::shared_ptr<Session> session,
                       std::optional<kota::cancellation_token> token = {});

    /// Go-to-definition, assembled across all providers: preamble directive
    /// targets, the index, and the worker's AST, with an index/directive
    /// retry after the dispatch's compile refreshes a dirty session.
    /// @param session may be null (document not open).
    /// @param token the request's cancellation token, forwarded to the
    /// worker sends (see Dispatcher::query).
    RawResult definition(std::shared_ptr<Session> session,
                         Fid path_id,
                         const protocol::Position& position,
                         std::optional<kota::cancellation_token> token = {});

    /// Whole-document features and hover, routed by readiness (see
    /// pick_route): the AST answers when current, the index projections
    /// answer while it is not, and a session the policy keeps un-compiled
    /// answers with its pinned degraded surface (empty inlay hints and
    /// code actions). Each takes the request's cancellation token and
    /// forwards it to the worker sends (see Dispatcher::query).
    RawResult hover(std::shared_ptr<Session> session,
                    const protocol::Position& position,
                    std::optional<kota::cancellation_token> token = {});
    RawResult semantic_tokens(std::shared_ptr<Session> session,
                              std::optional<kota::cancellation_token> token = {});
    RawResult inlay_hints(std::shared_ptr<Session> session,
                          const protocol::Range& range,
                          std::optional<kota::cancellation_token> token = {});
    RawResult folding_range(std::shared_ptr<Session> session,
                            std::optional<kota::cancellation_token> token = {});
    RawResult document_symbol(std::shared_ptr<Session> session,
                              std::optional<kota::cancellation_token> token = {});
    RawResult code_action(std::shared_ptr<Session> session,
                          std::optional<kota::cancellation_token> token = {});

    /// Code completion. Serves preamble contexts (include/import) locally from
    /// the include graph and module map; delegates ordinary code completion to
    /// a stateless worker. Pauses background indexing for the request's span.
    /// Space-triggered requests are only answered for import contexts.
    RawResult completion(std::shared_ptr<Session> session,
                         const protocol::Position& position,
                         llvm::StringRef trigger_character = {},
                         std::optional<kota::cancellation_token> token = {});

    /// Signature help, dispatched as a stateless build. Pauses background
    /// indexing for the request's span.
    RawResult signature_help(std::shared_ptr<Session> session,
                             const protocol::Position& position,
                             std::optional<kota::cancellation_token> token = {});

    /// Whole-document and range formatting on a stateless worker. Pause
    /// background indexing for the request's span.
    RawResult formatting(std::shared_ptr<Session> session,
                         std::optional<kota::cancellation_token> token = {});
    RawResult range_formatting(std::shared_ptr<Session> session,
                               const protocol::Range& range,
                               std::optional<kota::cancellation_token> token = {});

    /// Index navigation queries. Closed documents are fully serveable from the
    /// index and an empty result is a real answer (returned as []). @param
    /// session may be null (document not open) — the index is queried anyway.
    RawResult references(std::shared_ptr<Session> session,
                         Fid path_id,
                         const protocol::Position& position,
                         bool include_declaration);
    RawResult declaration(std::shared_ptr<Session> session,
                          Fid path_id,
                          const protocol::Position& position);
    RawResult type_definition(std::shared_ptr<Session> session,
                              Fid path_id,
                              const protocol::Position& position);
    RawResult implementation(std::shared_ptr<Session> session,
                             Fid path_id,
                             const protocol::Position& position);

    RawResult call_hierarchy_prepare(std::shared_ptr<Session> session,
                                     Fid path_id,
                                     const protocol::Position& position);
    RawResult call_hierarchy_incoming(std::shared_ptr<Session> session,
                                      Fid path_id,
                                      const protocol::CallHierarchyItem& item);
    RawResult call_hierarchy_outgoing(std::shared_ptr<Session> session,
                                      Fid path_id,
                                      const protocol::CallHierarchyItem& item);

    RawResult type_hierarchy_prepare(std::shared_ptr<Session> session,
                                     Fid path_id,
                                     const protocol::Position& position);
    RawResult type_hierarchy_supertypes(std::shared_ptr<Session> session,
                                        Fid path_id,
                                        const protocol::TypeHierarchyItem& item);
    RawResult type_hierarchy_subtypes(std::shared_ptr<Session> session,
                                      Fid path_id,
                                      const protocol::TypeHierarchyItem& item);

    RawResult workspace_symbol(llvm::StringRef query);

private:
    /// Whether the worker's AST can answer for this session right now:
    /// compiled, current, and not quarantined (the quarantine gate sits
    /// before ensure_compiled's clean-AST fast path, so a quarantined
    /// session's dispatch returns null even with a clean AST). When false,
    /// the routing rules try the index before deciding to await a compile.
    bool ast_answerable(const Session& session) const;

    /// The route decision for requests the index may serve: drains the
    /// transport pipe (the handler resumed eagerly; a didChange or cancel
    /// may be queued), then re-derives the source from current state.
    enum class Route : std::uint8_t {
        /// The ticket went stale while draining (didChange, close):
        /// answer ContentModified — the client re-requests against the
        /// new state.
        Superseded,
        /// Serve from the index source the freshness contract admits.
        Index,
        /// Fall through to the AST path (Dispatcher::query, which compiles
        /// as needed).
        Ast,
        /// No source can serve and none is being invested in (IndexOnly
        /// with no matching shard): answer honestly empty.
        Empty,
    };

    /// See Route. Sets up escalated sessions' compile kick so an index
    /// answer never strands the AST investment the policy asked for.
    /// `full_lex` marks projections that raw-lex the whole buffer
    /// (semantic tokens, folds): those follow the investment policy once
    /// the buffer is oversized, while row- and cursor-backed answers
    /// serve at any size. A Route::Index decision stores the admitted
    /// source into `source` (when given) — callers must serve from it
    /// rather than re-derive, or the state could shift between the
    /// decision and the read.
    kota::task<Route> pick_route(const Ticket& ticket,
                                 bool full_lex,
                                 ServingSource* source = nullptr);

    /// What a gate decided instead of the feature's own answer: null (no
    /// source can answer — a failed compile) or ContentModified.
    struct Stop {
        std::optional<kota::ipc::Error> error;
    };

    RawResult stop_reply(Stop stop);

    /// The compile gate of index-navigation requests: awaits the compile
    /// exactly when the routing decided the AST is the serving source
    /// (freshness clause 1 needs the settled file index), and lets
    /// index-served sessions resolve through clauses 1/4 without forcing
    /// the build. A Stop means answer with it instead: the compile failed,
    /// or the request was superseded.
    kota::task<std::optional<Stop>> nav_gate(const Ticket& ticket);

    /// The symbol under a position of the file's serving source, if any.
    std::optional<IndexQuery::Cursor> cursor_at(Fid path_id,
                                                const protocol::Position& position) const;

    /// The document's rows extracted for the projections, plus the
    /// resolver the projections share.
    struct IndexRows {
        std::vector<index::Occurrence> occurrences;
        std::vector<feature::IndexDeclRow> decls;
    };

    static IndexRows extract_rows(const index::Shard& shard);

    /// The lexing dialect of a session's index projections. An explicit -x
    /// in the file's own CDB entry decides outright; a header follows its
    /// active context's host when one exists; otherwise C when the file is
    /// a C source or every TU contributing its indexed rows is one, C++
    /// else (mixed inclusion favors the superset).
    const clang::LangOptions& index_lang_options(const Session& session);

    /// The read-only hover card for the symbol under the cursor: name and
    /// kind from the symbol tables, definition text sliced from stored
    /// content, the comment block above the definition. No Sema products
    /// — see feature::index_hover.
    std::optional<feature::HoverInfo> index_hover_card(const Session& session,
                                                       const protocol::Position& position);

    /// The preamble include links of a session's active PCH; empty when
    /// there is no PCH or its preamble no longer matches the buffer.
    std::vector<feature::DocumentLink> find_preamble_links(const Session& session);

    /// Resolve go-to-definition on a preamble include line that the worker
    /// AST cannot see: the include is compiled into the PCH, so the target
    /// is answered from the PCH's cached preamble links. Module names go
    /// through the ordinary index pipeline, not this path.
    std::vector<protocol::Location>
        resolve_directive_definition(Session& session, const protocol::Position& position);

    /// Resolve hover on a preamble include from the links cached with the PCH.
    std::optional<protocol::Hover> resolve_preamble_hover(Session& session,
                                                          const protocol::Position& position);

    ASTFamily& ast;
    Dispatcher& dispatcher;
    IndexQuery& query;
    Workspace& workspace;
    ContextResolver& contexts;
    IndexPump& pump;
    SessionStore& sessions;
};

}  // namespace clice

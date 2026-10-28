module;

#include "support/prelude.h"

module clice:server.requests;

import :feature.code_completion;
import :feature.diagnostics;
import :feature.document_symbols;
import :feature.folding_ranges;
import :feature.hover;
import :feature.rename;
import :feature.semantic_tokens;
import :feature.workspace_symbols;
import :index.peer_queries;
import :index.query;
import :project.project;
import :sched.index.pump;
import :server.ast_family;
import :server.dispatcher;
import :server.editor_context;
import :server.session;
import :server.session_store;
import :syntax.source_text;

namespace clice {

/// The server's language features, each assembled from its providers.
///
/// A feature answer can come from three sources:
///   - the live AST: dispatched to the worker that holds the file's AST;
///   - build-companion caches: products derived while building the PCH
///     (e.g. the preamble's document links), cached master-side because the
///     worker's AST is built on top of the PCH and cannot see the preamble
///     region;
///   - the index: every index source, read through index::IndexQuery.
///
/// Routing is derived per request from readiness, never from a mode
/// flag: a current AST answers as today; otherwise an index source that
/// is byte-identical to the buffer answers immediately (whole-document
/// features through the feature::index_* functions, cursor queries
/// through index::IndexQuery's freshness clauses); otherwise the request awaits
/// the compile the policy owes, or answers honestly empty when it owes
/// none (ServingMode::IndexOnly).
///
/// Every entry takes its Ticket before its first suspension, drains the
/// transport pipe, and answers ContentModified once the ticket goes
/// stale: the client keeps what it has and re-pulls. The exceptions are
/// completion, which deliberately serves the drained buffer, and
/// diagnostics, which follow the buffer (see there).
///
/// Discipline: any feature whose answer is assembled from more than one
/// source belongs here. Transports (LSP handlers) only translate
/// between the protocol and these methods — they never merge, retry,
/// or gate results themselves.
class Requests {
public:
    Requests(ASTFamily& ast,
             Dispatcher& dispatcher,
             index::IndexQuery& query,
             Project& project,
             EditorContext& contexts,
             IndexPump& pump,
             SessionStore& sessions,
             const PositionEncoding& encoding) :
        ast(ast), dispatcher(dispatcher), query(query), project(project), contexts(contexts),
        pump(pump), sessions(sessions), encoding(encoding) {}

    using RawResult = Dispatcher::RawResult;

    /// The other projects' index queries, and the query of the project a
    /// file open in the editor is routed to (null for a closed file), both
    /// connected by the master (see index::PeerQueries).
    std::function<llvm::SmallVector<const index::IndexQuery*>()> peers;
    std::function<const index::IndexQuery*(Fid)> open_in;

    /// The editor's requests take their ticket as the request is dispatched,
    /// ahead of the messages read with it: an edit among those arrives before
    /// the task starts and supersedes the ticket.

    /// Full document-link result for a session: the worker's focus-file links
    /// merged behind the PCH's cached preamble links.
    kota::task<std::vector<protocol::DocumentLink>, ipc::Error>
        document_links(Ticket ticket, kota::cancellation_token token = {});

    /// The diagnostics of the document's buffer as it is now: awaits the
    /// compile the buffer is owed, and an edit arriving meanwhile is
    /// followed rather than answered ContentModified — a pull names no
    /// version, and clients clear what they hold on that error. Following
    /// has no bound of its own: a client that no longer wants the answer
    /// cancels the pull. A session the policy keeps un-compiled waits for
    /// nothing.
    kota::task<std::vector<feature::FileDiagnostic>> diagnostics(std::shared_ptr<Session> session);

    /// What diagnostics answers once the compile finished, without waiting
    /// for one: the output's diagnostics while it describes the buffer,
    /// then the crash notes; nothing once the session closed.
    std::vector<feature::FileDiagnostic> finished_diagnostics(const Session& session) const;

    /// Go-to-definition, assembled across all providers: preamble directive
    /// targets, the index, and the directives the worker's AST sees, with
    /// an index retry after the dispatch's compile refreshes a dirty
    /// session.
    /// @param ticket has no session for a document not open.
    /// @param token the request's cancellation token, forwarded to the
    /// worker sends (see Dispatcher::query).
    RawResult definition(Ticket ticket,
                         Fid path_id,
                         const protocol::Position& position,
                         kota::cancellation_token token = {});

    /// Whole-document features and hover, routed by readiness (see
    /// pick_route): the AST answers when current, the results computed
    /// from the index answer while it is not, and a session the policy keeps un-compiled
    /// answers with its pinned degraded surface (empty inlay hints). Each
    /// takes the request's cancellation token and forwards it to the
    /// worker sends (see Dispatcher::query).
    RawResult hover(Ticket ticket,
                    const protocol::Position& position,
                    protocol::MarkupKind markup,
                    kota::cancellation_token token = {});
    /// The document's semantic tokens, none when nothing serves them yet;
    /// the caller encodes them against the session's text right away.
    using TokensResult = kota::task<std::optional<std::vector<feature::SemanticToken>>, ipc::Error>;
    TokensResult semantic_tokens(Ticket ticket, kota::cancellation_token token = {});
    RawResult inlay_hints(Ticket ticket,
                          const protocol::Range& range,
                          bool label_parts,
                          kota::cancellation_token token = {});
    RawResult folding_ranges(Ticket ticket,
                             feature::FoldingClient client,
                             kota::cancellation_token token = {});
    /// A flat reply places every symbol at `uri`, the document as the
    /// request names it.
    RawResult document_symbols(Ticket ticket,
                               feature::SymbolClient client,
                               std::string uri,
                               kota::cancellation_token token = {});

    /// One chain of selection ranges per position: from the AST, or for a
    /// document without one from the raw lex of its text.
    RawResult selection_ranges(Ticket ticket,
                               std::vector<protocol::Position> positions,
                               kota::cancellation_token token = {});

    /// Code actions on a range of the buffer: the worker computes them to
    /// completion against its AST, and the index requests they carry
    /// (definitions to vet and place in the host source, headers
    /// declaring a name) are resolved here, at the reply edge. Every edit
    /// is versioned against the buffer it was computed for. `only` is the
    /// client's kind filter (LSP CodeActionContext.only); empty keeps all.
    kota::task<std::vector<protocol::CodeAction>, ipc::Error>
        code_actions(Ticket ticket,
                     const protocol::Range& range,
                     llvm::ArrayRef<protocol::CodeActionKind> only,
                     kota::cancellation_token token = {});

    /// Code completion. Serves preamble contexts (include/import) locally from
    /// the include graph and module map; delegates ordinary code completion to
    /// a stateless worker. Pauses background indexing for the request's span.
    /// Space-triggered requests are only answered for import contexts.
    RawResult code_completion(std::shared_ptr<Session> session,
                              const protocol::Position& position,
                              const feature::CompletionClient& client,
                              llvm::StringRef trigger_character = {},
                              kota::cancellation_token token = {});

    /// Signature help, dispatched as a stateless build. Pauses background
    /// indexing for the request's span.
    /// `selected` is the label of the signature the client shows active.
    RawResult signature_help(std::shared_ptr<Session> session,
                             const protocol::Position& position,
                             std::string selected,
                             kota::cancellation_token token = {});

    /// Whole-document formatting, or a range's, on a stateless worker.
    /// Pauses background indexing for the request's span.
    RawResult formatting(std::shared_ptr<Session> session,
                         std::optional<protocol::Range> range,
                         kota::cancellation_token token = {});

    /// Index navigation queries. Closed documents are fully serveable from the
    /// index and an empty result is a real answer (returned as []). @param
    /// ticket has no session for a document not open — the index is queried
    /// anyway.
    RawResult references(Ticket ticket,
                         Fid path_id,
                         const protocol::Position& position,
                         bool include_declaration);
    RawResult document_highlights(Ticket ticket, Fid path_id, const protocol::Position& position);
    RawResult declaration(Ticket ticket, Fid path_id, const protocol::Position& position);
    RawResult type_definition(Ticket ticket, Fid path_id, const protocol::Position& position);
    RawResult implementation(Ticket ticket, Fid path_id, const protocol::Position& position);

    RawResult call_hierarchy_prepare(Ticket ticket,
                                     Fid path_id,
                                     const protocol::Position& position);
    RawResult call_hierarchy_incoming(Fid path_id, const protocol::CallHierarchyItem& item);
    RawResult call_hierarchy_outgoing(Fid path_id, const protocol::CallHierarchyItem& item);

    RawResult type_hierarchy_prepare(Ticket ticket,
                                     Fid path_id,
                                     const protocol::Position& position);
    RawResult type_hierarchy_supertypes(Fid path_id, const protocol::TypeHierarchyItem& item);
    RawResult type_hierarchy_subtypes(Fid path_id, const protocol::TypeHierarchyItem& item);

    /// Rename from this project's index (see feature/rename.cppm), never a
    /// recompile. prepare_rename answers the name token at the position,
    /// or an error saying why the symbol there cannot be renamed.
    RawResult prepare_rename(Ticket ticket, Fid path_id, const protocol::Position& position);

    /// Nullopt when nothing at the position can be renamed or the file
    /// fails to compile; an error names the conflicts, or the files whose
    /// index is not current, that keep the rename from being made.
    kota::task<std::optional<feature::Renamed>, ipc::Error>
        rename(Ticket ticket,
               Fid path_id,
               const protocol::Position& position,
               std::string new_name);

    /// The project's symbols matching `query`, best first; the master
    /// merges every project's list.
    std::vector<feature::WorkspaceSymbol> workspace_symbols(llvm::StringRef query);

private:
    /// Whether the worker's AST can answer for this session right now:
    /// compiled, current, and its compile not barred by a crash (the bar
    /// sits before ensure_compiled's clean-AST fast path, so a barred
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

    struct RouteOptions {
        /// The feature's answer from the index raw-lexes the whole buffer
        /// (semantic tokens, folds, the outline): it follows the investment
        /// policy once the buffer is oversized, while answers that only look
        /// the index up serve at any size.
        bool full_lex = false;
        /// The feature has no refresh request, so an empty reply to a cold
        /// document would be cached by the client for good — no signal
        /// ever makes it re-pull. Await the didOpen boost once (bounded by
        /// one index attempt) before answering Empty, and decide again
        /// from whatever source is there once it finishes: the shard, the escalated
        /// compile, or honestly empty under readonly "on".
        bool await_cold_attempt = false;
    };

    /// See Route. Sets up escalated sessions' compile kick so an index
    /// answer never strands the AST investment the policy asked for. A
    /// Route::Index decision stores the admitted source into `source`
    /// (when given) — callers must serve from it rather than re-derive,
    /// or the state could shift between the decision and the read.
    kota::task<Route> pick_route(const Ticket& ticket,
                                 RouteOptions options,
                                 std::optional<index::IndexSource>* source = nullptr);

    /// completion's task, which takes its ticket as it starts.
    RawResult complete(std::shared_ptr<Session> session,
                       IndexPump::ScopedPause pause,
                       const protocol::Position& position,
                       const feature::CompletionClient& client,
                       llvm::StringRef trigger_character,
                       kota::cancellation_token token);

    /// What a gate decided instead of the feature's own answer: null (no
    /// source can answer — a failed compile) or ContentModified.
    struct Stop {
        std::optional<ipc::Error> error;
    };

    RawResult stop_reply(Stop stop);

    /// The compile gate of index-navigation requests: awaits the compile
    /// exactly when the routing decided the AST is the serving source
    /// (freshness clause 1 needs the finished file index), and lets
    /// index-served sessions resolve through clauses 1/4 without forcing
    /// the build. A Stop means answer with it instead: the compile failed,
    /// or the request was superseded.
    kota::task<std::optional<Stop>> nav_gate(const Ticket& ticket);

    /// Await the session's compile; a Stop when it failed or the request
    /// was superseded.
    kota::task<std::optional<Stop>> await_compile(const Ticket& ticket);

    /// The symbol under a position of the file's serving source, if any.
    std::optional<index::IndexQuery::Cursor> cursor_at(Fid path_id,
                                                       const protocol::Position& position) const;

    /// This project's query and its peers', for one synchronous read: the
    /// peers are those the master serves when it is taken.
    index::PeerQueries peer_queries() const;

    /// The session's text, read in the dialect the server lexes it in
    /// (EditorContext::dialect).
    SourceText& text_of(Session& session);

    /// The card the index gives for the symbol at a position of the
    /// session's serving source (feature::hover_info).
    std::optional<feature::HoverInfo> index_hover_card(const Session& session,
                                                       const protocol::Position& position);

    /// The preamble include links of a session's active PCH; empty when
    /// there is no PCH or its preamble no longer matches the buffer.
    std::vector<index::DocumentLink> find_preamble_links(const Session& session);

    /// The include-like directive links of a session's whole buffer: the
    /// PCH's cached preamble links, then the worker's AST's for the rest.
    /// Module names go through the ordinary index pipeline, not these.
    kota::task<std::vector<index::DocumentLink>, ipc::Error>
        directive_links(const Ticket& ticket, kota::cancellation_token token);

    ASTFamily& ast;
    Dispatcher& dispatcher;
    index::IndexQuery& query;
    Project& project;
    EditorContext& contexts;
    IndexPump& pump;
    SessionStore& sessions;

    /// What the client's positions count in (MasterServer::position_encoding).
    const PositionEncoding& encoding;
};

}  // namespace clice

module clice:server.dispatcher;

import :feature.code_actions;
import :feature.code_completion;
import :feature.document_symbols;
import :feature.folding_ranges;
import :feature.hover;
import :feature.inlay_hints;
import :feature.semantic_tokens;
import :server.include_trees;
import :server.ast_family;
import :server.document_failures;
import :server.editor_context;
import :server.session;
import :support.prelude;
import :worker.pool;
import :worker.protocol;

namespace clice {

/// LSP's ContentModified: the request's answer would describe a buffer the
/// client already moved past. Clients keep what they have and re-pull —
/// unlike a null, which a client is entitled to read as "there is nothing"
/// (VS Code clears semantic highlighting on one).
constexpr inline auto content_modified_code =
    static_cast<protocol::integer>(protocol::LSPErrorCodes::ContentModified);

ipc::Error content_modified();

/// The document side of talking to workers. Every request that carries an
/// open document's content to a worker — or asks the worker holding its
/// AST — goes through here and finishes through one exit. The pool owns the
/// worker side (slots, crash budgets, routing, attributing a death); this
/// owns what a request means for the document: whether its failure records
/// admit it, whose crash a death is, and whether the reply still describes
/// the buffer.
///
/// One check admits a dispatch, of every kind (admits): before its inputs
/// are prepared, and again where it reaches a worker — after a stateless
/// request's wait in the queue and before every resend — since a request
/// like it can crash on the same content meanwhile. The single exit holds
/// the other invariant: a reply is handed back only while the ticket is
/// fresh, and only a fresh reply clears the document's record for its
/// kind. A stale reply becomes ContentModified before any caller can see
/// it — no path returns a result for a buffer that no longer exists. A
/// request the workers could not serve — stopped by a crash, or lost to
/// deaths and outages — answers empty: the crash note on the document says
/// why, and an error would only show as a client popup.
class Dispatcher {
public:
    Dispatcher(Project& project,
               EditorContext& contexts,
               ASTFamily& ast,
               IncludeTrees& trees,
               WorkerPool& pool,
               DocumentFailures& failures,
               const PositionEncoding& encoding);

    using RawResult = kota::task<codec::RawValue, ipc::Error>;

    /// The hover card at a position of the buffer the ticket was taken on,
    /// before it is rendered.
    kota::task<std::optional<feature::HoverInfo>, ipc::Error>
        hover(const Ticket& ticket,
              const protocol::Position& position,
              kota::cancellation_token token = {});

    /// The inlay hints of the open file's AST within `range` of the buffer
    /// the ticket was taken on.
    kota::task<std::vector<feature::InlayHint>, ipc::Error>
        inlay_hints(const Ticket& ticket,
                    const protocol::Range& range,
                    kota::cancellation_token token = {});

    /// The focus file's document links from the stateful worker holding the
    /// AST; the preamble's live in the PCH's TU index (see
    /// PCHState::load_state).
    kota::task<std::vector<index::DocumentLink>, ipc::Error>
        document_links(const Ticket& ticket, kota::cancellation_token token = {});

    /// The folding ranges from the stateful worker holding the AST, none
    /// without one.
    kota::task<std::optional<std::vector<feature::FoldingRange>>, ipc::Error>
        folding_ranges(const Ticket& ticket, kota::cancellation_token token = {});

    kota::task<std::optional<std::vector<feature::DocumentSymbol>>, ipc::Error>
        document_symbols(const Ticket& ticket, kota::cancellation_token token = {});

    kota::task<std::optional<std::vector<feature::SemanticToken>>, ipc::Error>
        semantic_tokens(const Ticket& ticket, kota::cancellation_token token = {});

    /// The selection ranges around offsets of the buffer from the stateful
    /// worker holding the AST, none without one.
    kota::task<std::vector<std::vector<LocalSourceRange>>, ipc::Error>
        selection_ranges(const Ticket& ticket,
                         std::vector<std::uint32_t> offsets,
                         kota::cancellation_token token = {});

    /// The code actions on a range of the buffer, from the stateful worker
    /// holding the AST; index requests come back unresolved.
    kota::task<std::vector<feature::CodeAction>, ipc::Error>
        code_actions(const Ticket& ticket,
                     const protocol::Range& range,
                     kota::cancellation_token token = {});

    /// The interactive stateless builds: the buffer and its compile inputs
    /// go to a stateless worker, which compiles a buffer state the shared
    /// AST round may not have seen yet.
    kota::task<worker::CompletionResult, ipc::Error>
        completion(const Ticket& ticket,
                   const protocol::Position& position,
                   const feature::CompletionClient& client,
                   std::vector<feature::ScopeCandidate> candidates,
                   bool more_candidates,
                   kota::cancellation_token token = {});
    RawResult signature_help(const Ticket& ticket,
                             const protocol::Position& position,
                             std::string selected,
                             kota::cancellation_token token = {});

    /// Formatting on a stateless worker: no sema runs, but it is still this
    /// document's content on a worker. None while a crash stops it.
    kota::task<std::optional<worker::FormatResult>, ipc::Error>
        format(const Ticket& ticket,
               std::optional<protocol::Range> range = {},
               kota::cancellation_token token = {});

private:
    /// Shared body of the requests to the worker holding the AST: the
    /// compile, the gate, the send and the finish; a missing AST answers an
    /// empty result. `token`, on every dispatch: the LSP request's
    /// cancellation token. Passing it into the worker send turns a client
    /// $/cancelRequest into an IPC cancel — the worker stops the parse at
    /// the next top-level declaration instead of computing a result nobody
    /// will read. The shared compile a query waits on is deliberately NOT
    /// cancelled: it serves every waiter, not this request.
    template <typename Params>
    kota::task<typename protocol::RequestTraits<Params>::Result, ipc::Error>
        ast_request(const Ticket& ticket,
                    feature::RequestKind kind,
                    llvm::StringRef label,
                    Params params,
                    kota::cancellation_token token);

    /// Shared body of the interactive builds: identical inputs and
    /// admission, different IPC type, request kind and label. `wp` arrives
    /// with the fields particular to its request filled in.
    template <typename Params>
    kota::task<typename protocol::RequestTraits<Params>::Result, ipc::Error>
        interactive(feature::RequestKind kind,
                    llvm::StringRef label,
                    const Ticket& ticket,
                    protocol::Position position,
                    Params wp,
                    kota::cancellation_token token);

    /// Send an AST query of `kind` to the worker holding the document,
    /// which the caller compiled. A resend after a worker death recompiles
    /// first — the AST died with the worker — and a worker that no longer
    /// holds the document answers document_unloaded, which compiles it
    /// there and asks once more. `unanswered` reports a query never sent:
    /// its kind stopped by a crash, or a recompile that produced no AST.
    template <typename Params>
    RequestResult<Params> ask(const Ticket& ticket,
                              feature::RequestKind kind,
                              const Params& params,
                              kota::cancellation_token token,
                              bool& unanswered);

    /// Send a stateless request of `kind` carrying the document's content,
    /// which `attempt` holds from its first admission on: admitted again
    /// once it has its slot and before every resend. `refused` reports one
    /// never sent. `parses`: see admits.
    template <typename Params>
    RequestResult<Params> send(const Ticket& ticket,
                               feature::RequestKind kind,
                               const Params& params,
                               const DocumentFailures::Attempt& attempt,
                               bool parses,
                               kota::cancellation_token token,
                               bool& refused);

    /// Whether a dispatch of `kind` may reach a worker: its record does
    /// not stop it — or, once an attempt holds it, `kind` did not crash
    /// during its flight — and for a build that parses the document
    /// (`parses`), neither does the compile's or its preamble's.
    bool admits(Fid path_id,
                feature::RequestKind kind,
                const DocumentFailures::Attempt* attempt,
                bool parses) const;

    /// The single exit of every dispatch: a fresh reply clears the kind's
    /// failure record, a stale one never leaves as a value — unless it is
    /// a `snapshot` reply, which describes the buffer the request carried
    /// and which the client reconciles with the edits made meanwhile. A
    /// fresh dispatch the workers could not serve answers empty.
    template <typename Outcome>
    Outcome finish(const Ticket& ticket,
                   feature::RequestKind kind,
                   llvm::StringRef label,
                   Outcome result,
                   bool snapshot = false);

    Project& project;
    EditorContext& contexts;
    ASTFamily& ast;
    IncludeTrees& trees;
    WorkerPool& pool;
    DocumentFailures& failures;

    /// What the client's positions count in (MasterServer::position_encoding).
    const PositionEncoding& encoding;
};

}  // namespace clice

module;

#include "support/prelude.h"

module clice:server.lsp_client;

import :feature.code_completion;
import :feature.diagnostics;
import :feature.document_symbols;
import :feature.folding_ranges;
import :feature.semantic_tokens;
import :server.ast_family;
import :server.extension;
import :server.master_server;
import :server.project_server;
import :server.session;
import :support.signal;
import :vfs.file_table;

namespace clice {

class LSPClient {
public:
    LSPClient(MasterServer& server, ipc::JSONPeer& peer);
    ~LSPClient();

private:
    using RawResult = kota::task<codec::RawValue, ipc::Error>;

    /// Shared front half of every document-addressed handler: URI → path →
    /// interned path_id → open session (null when the document is not open)
    /// → the project serving it, held for the request: a folder removed
    /// while the request runs keeps its project alive until it completes.
    struct ResolvedDoc {
        std::string path;
        Fid path_id;
        std::shared_ptr<Session> session;
        std::shared_ptr<ProjectServer> project;
    };

    ResolvedDoc resolve_uri(const std::string& uri);

    /// LSP handler registration, grouped by category. Each installs its
    /// handlers on `peer`; all four are invoked once from the constructor.
    void register_lifecycle();
    void register_document_sync();
    void register_language_features();
    void register_extensions();

    /// clice/switchContext, clice/resetContext and clice/switchConfiguration,
    /// which an editor asks for directly or through the commands of the
    /// source actions switching them (switch_actions).
    kota::task<ext::SwitchContextResult> switch_context(const ext::SwitchContextParams& params);
    ext::SwitchContextResult reset_context(const ext::ResetContextParams& params);
    ext::SwitchConfigurationResult
        switch_configuration(const ext::SwitchConfigurationParams& params);

    /// The project serving the named document; without one the first
    /// project over a folder, the first one being rootless in a server
    /// without folders.
    std::shared_ptr<ProjectServer> configured(const std::optional<std::string>& uri);

    /// Source actions switching what the document compiles under, for an
    /// editor whose only UI for it is its code action menu (Zed, Helix):
    /// every other context of the listing's first page, the automatic one
    /// while the user's choice is in force, and every other build
    /// configuration. Their commands run through workspace/executeCommand.
    std::vector<protocol::CodeAction> switch_actions(ProjectServer& project,
                                                     const Session& session,
                                                     llvm::ArrayRef<protocol::CodeActionKind> only);
    RawResult execute_command(protocol::ExecuteCommandParams params);

    /// Ask the client to pull every document's diagnostics again;
    /// capability-gated.
    void refresh_diagnostics();

    /// Push every served project's clice.toml load problems as
    /// diagnostics on the config file URI, and clear the ones of a
    /// configuration no project loads any more.
    void publish_config_diagnostics();

    /// Push a session's materialized compile output to the client: its
    /// diagnostics — or, to a pulling client, a diagnostics refresh when an
    /// answer it holds went stale — plus the refresh requests its arrival
    /// warrants. Invoked by the compiler's on_output signal, and by the
    /// initialized handler to replay outputs that materialized before the
    /// client was ready. No-op until client_ready. An output that changed
    /// with no compile (`compiled` false: the on_diagnostics signal) pushes
    /// its diagnostics alone.
    void push_output(ProjectServer& project, const Session& session, bool compiled);

    /// React to a background-indexing progress change: drive the LSP
    /// work-done progress token through its begin/report/end lifecycle,
    /// reading the counts from MasterServer::index_progress. Invoked by
    /// the server's on_index_progress signal.
    void report_index_progress();

    /// Ask the client to re-pull semantic tokens and folding ranges after
    /// a background merge replaced a shard an index-served session was
    /// serving. Invoked by MasterServer's on_serving_index_changed signal;
    /// capability-gated.
    void refresh_index_served();

    /// Ask the client to request again what it holds of documents whose
    /// inputs changed on disk: a pulling client pulls the diagnostics, any
    /// other asks for semantic tokens, and either request recompiles the
    /// document, whose diagnostics then reach it. Invoked by
    /// MasterServer's on_inputs_changed signal; capability-gated.
    void refresh_stale();

    /// Send every not-yet-forwarded notify-log message, advancing
    /// notify_cursor: guidance as window/showMessage, for the user to act
    /// on, an anomaly as window/logMessage. Invoked once the client is
    /// ready (a headless server may have accumulated messages), then by the
    /// server's on_notify signal.
    void forward_notify_messages();

    /// Follow a document's compile to `phase` with work-done progress (see
    /// CompileProgress). Invoked by the server's on_compile_phase signal;
    /// capability-gated.
    void report_compile_phase(Fid path_id, CompilePhase phase);

    MasterServer& server;
    ipc::JSONPeer& peer;

    /// The client completed the initialized handshake. Until then the LSP
    /// spec forbids server→client traffic other than window/* messages, so
    /// diagnostics pushes and progress-token creation are held back;
    /// compile outputs missed in that window are replayed by the
    /// initialized handler from the sessions' materialized output state.
    bool client_ready = false;

    /// Whether the client accepts workspace/semanticTokens/refresh,
    /// workspace/inlayHint/refresh and workspace/foldingRange/refresh —
    /// the upgrade signal after an index-served session's compile finishes
    /// (see push_output).
    bool semantic_tokens_refresh = false;
    bool inlay_hint_refresh = false;
    bool folding_range_refresh = false;

    /// Whether the client accepts workspace/diagnostic/refresh, the
    /// re-pull signal for a pulled answer gone stale with no edit of its
    /// own (see pulled).
    bool diagnostic_refresh = false;

    /// Whether the client takes a diagnostic's notes as related
    /// information; otherwise they go into its message.
    bool related_information = false;

    /// Whether the client takes work-done progress the server starts
    /// (window.workDoneProgress).
    bool work_done_progress = false;

    /// The diagnostics in the shape the client takes.
    std::vector<protocol::Diagnostic>
        to_client(llvm::ArrayRef<feature::FileDiagnostic> diagnostics);

    /// How the client takes folds (textDocument.foldingRange).
    feature::FoldingClient folding_client;

    /// The legend of the semantic tokens, fixed when the client initializes.
    feature::SemanticTokenLegend token_legend;

    /// What a semantic tokens request asks for: the whole document, the
    /// edits from the result the client holds, or the tokens in a range.
    struct TokensRequest {
        std::optional<std::string> previous;
        std::optional<protocol::Range> window;
    };

    RawResult send_tokens(std::shared_ptr<ProjectServer> project,
                          Fid path_id,
                          std::shared_ptr<Session> session,
                          ipc::JSONPeer::RequestContext& ctx,
                          TokensRequest request);

    /// The last full or delta result sent for each open document, which
    /// the next delta request names.
    struct SentTokens {
        std::string result_id;
        std::vector<std::uint32_t> data;
    };

    llvm::DenseMap<Fid, SentTokens> sent_tokens;
    std::uint64_t next_result_id = 0;

    /// How the client takes document symbols (textDocument.documentSymbol).
    feature::SymbolClient symbol_client;

    /// The client takes inlay hint labels in parts (LSP 3.17
    /// textDocument.inlayHint): the names in them link to their symbols.
    bool label_parts = false;

    /// The client pulls diagnostics (the textDocument.diagnostic
    /// capability): an open document's are answered to its pulls and never
    /// pushed — clients keep pushed and pulled diagnostics apart and would
    /// show both. Pull wins whenever the client offers it: a compile runs
    /// only when a request needs the AST, so a pull is itself the request
    /// that brings the diagnostics up to date, while a client that syncs
    /// edits and waits for a push gets none until something else asks.
    /// Configuration files are still pushed; no client pulls them.
    bool pull_diagnostics = false;

    /// What the client takes from a completion item.
    feature::CompletionClient completion_client;

    /// The markup the client shows hover cards in: markdown when its
    /// textDocument.hover.contentFormat lists it.
    protocol::MarkupKind hover_markup = protocol::MarkupKind::PlainText;

    /// Whether the client applies versioned document changes (the
    /// workspace/workspaceEdit.documentChanges capability): code action
    /// edits then carry the buffer version they were computed for, and
    /// a client refuses to apply them to a buffer that moved on.
    bool versioned_edits = false;

    /// Document version of each path's last compile output (see
    /// push_output): an output arriving for the same version means the text
    /// did not change, so the client's pulled results went stale without
    /// any didChange to make it re-pull — the push path sends refreshes.
    llvm::DenseMap<Fid, int> output_versions;

    /// The last diagnostics report pulled per path, and the buffer version
    /// it was answered at. While the buffer stays at that version nothing
    /// makes the client pull again: an output changing the report has
    /// push_output ask it to. Comparing the reports, not only the version,
    /// keeps a compile failing the same way on every attempt from
    /// refreshing forever.
    struct Pulled {
        int version;
        /// xxh3 of the report.
        std::uint64_t report;
    };

    llvm::DenseMap<Fid, Pulled> pulled;

    /// A document naming a file already open under another name: the first
    /// name owns the file's buffer, this one keeps its own text and takes
    /// over when the owner closes. While both texts are equal it shares the
    /// first name's answers; diverged, it gets none, and the user is told
    /// to close one.
    struct AliasDocument {
        std::string spelling;
        Session buffer;

        /// The user was told the texts diverged; reset once they agree.
        bool warned = false;
    };

    llvm::DenseMap<Fid, llvm::SmallVector<AliasDocument, 1>> aliases;

    /// The alias document of `path_id` spelled `spelling`, or nullptr.
    AliasDocument* find_alias(Fid path_id, llvm::StringRef spelling);

    /// The alias document spelled `spelling`, or nullptr.
    AliasDocument* find_alias(llvm::StringRef spelling);

    /// Take `alias`, one of `path_id`'s, out of the waiting list.
    AliasDocument take_alias(Fid path_id, AliasDocument* alias);

    /// Why a request on a document has no session to answer it.
    ipc::Error unserved(llvm::StringRef spelling);

    /// What a second name tells the user while its text differs from the
    /// first name's.
    std::string divergence_message(const AliasDocument& alias);

    /// Publish a second name's diagnostics: the first name's while their
    /// texts agree, else one telling the user to close either. A pulling
    /// client gets them from its pulls, and is asked to pull again when the
    /// texts part or meet again.
    void publish_alias(AliasDocument& alias, const Session* owner, ProjectServer& project);

    /// publish_alias for every second name of `path_id`.
    void publish_aliases(Fid path_id);

    /// The configuration files publish_config_diagnostics published last.
    llvm::StringSet<> published_configs;

    /// Subscription to compile outputs; disconnects on destruction.
    Signal<ProjectServer&, std::shared_ptr<Session>>::Connection output_conn;

    /// Subscription to diagnostics changing with no compile; disconnects on
    /// destruction.
    Signal<ProjectServer&, std::shared_ptr<Session>>::Connection diagnostics_conn;

    /// Subscription to the served projects changing; disconnects on
    /// destruction.
    Signal<>::Connection projects_conn;

    /// Subscription to background-index progress; disconnects on destruction.
    Signal<>::Connection progress_conn;

    /// Subscription to serving-index replacements by background merges;
    /// disconnects on destruction.
    Signal<>::Connection serving_conn;

    /// Subscription to open documents' inputs changing on disk; disconnects
    /// on destruction.
    Signal<>::Connection inputs_conn;

    /// Subscription to guidance/anomaly message wake-ups; disconnects on
    /// destruction.
    Signal<>::Connection notify_conn;

    /// Subscription to the test hook's parked replies (clice/internal/held);
    /// disconnects on destruction.
    Signal<std::uint64_t>::Connection held_conn;

    /// Subscription to open documents' compile phases; disconnects on
    /// destruction.
    Signal<Fid, CompilePhase>::Connection compile_phase_conn;

    /// Sequence number of the next notify-log message to forward (see
    /// MasterServer::notify_seq).
    std::uint64_t notify_cursor = 0;

    /// The guidance shown to the user, each text once.
    llvm::StringSet<> shown_guidance;

    /// Progress-token lifecycle, split into orthogonal facts so the
    /// asynchronous create() handshake can reconcile against rounds that
    /// begin or end while it is in flight. At most one create() is ever
    /// outstanding, and the reporter is never replaced while a handshake
    /// coroutine is awaiting on it.
    ///
    /// Held behind a shared_ptr because the handshake runs as a detached
    /// task on the event loop: the task captures this state (never the
    /// LSPClient), so a connection torn down mid-handshake cannot leave the
    /// task dereferencing a destroyed client — it observes `abandoned` and
    /// stops.
    struct IndexProgressState {
        /// A create() handshake is awaiting the client's acknowledgement.
        bool create_inflight = false;

        /// begin() has been announced on the token; reports may flow.
        bool token_active = false;

        /// An indexing round is running (Begin seen, End not yet).
        bool round_active = false;

        /// The owning LSPClient was destroyed; the handshake must not
        /// announce the token or touch the peer.
        bool abandoned = false;

        /// Total file count captured when the round began, for the begin
        /// message.
        std::uint32_t total = 0;

        /// The active work-done progress token, held across
        /// begin/report/end.
        std::optional<lsp::ProgressReporter<ipc::JSONPeer>> reporter;
    };

    std::shared_ptr<IndexProgressState> index_progress = std::make_shared<IndexProgressState>();

    /// The progress of one document's compile. It is announced only once the
    /// compile has run a while, so the quick compile after every edit shows
    /// none, and a compile starting while the announcement is pending joins
    /// it. Shared with the task announcing it, which reads `abandoned` before
    /// touching the client after each of its waits.
    struct CompileProgress {
        /// What the compile waits on; Idle once it ended.
        CompilePhase phase = CompilePhase::Idle;

        /// The owning LSPClient was destroyed.
        bool abandoned = false;

        /// Set once announced: reports and the end go through it.
        std::optional<lsp::ProgressReporter<ipc::JSONPeer>> reporter;
    };

    /// The documents whose compile runs or whose announcement is pending.
    llvm::DenseMap<Fid, std::shared_ptr<CompileProgress>> compile_progress;

    /// Wait out the delay, then announce `state`'s compile unless it ended.
    kota::task<> announce_compile(std::shared_ptr<CompileProgress> state, Fid path_id);
};

}  // namespace clice

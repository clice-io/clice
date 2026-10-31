module clice:server.client;

import :feature.code_completion;
import :feature.diagnostics;
import :feature.document_symbols;
import :feature.folding_ranges;
import :feature.semantic_tokens;
import :project.project;
import :server.extension;
import :server.master;
import :server.served_project;
import :server.session;
import :support.prelude;
import :support.signal;
import :syntax.position;
import :vfs.file_table;

namespace clice {

/// How a request on a document is served (LSPClient::on_document).
struct DocumentRequestOptions {
    /// A document that is not open is served too, from the index.
    bool closed = false;
};

/// The connection to the editor: the protocol's conversions, what the client
/// declared it takes, and what it holds of the server's answers. The
/// document notifications go to the MasterServer, and the server calls back
/// to push and refresh; each language request is answered in its handler,
/// asking the project serving the document what answers it now and asking
/// the workers (ServedProject::pick_route, ServedProject::ast_request). Its
/// work is split in files: the handshake (handshake.cpp), the document
/// notifications (sync.cpp), the document's requests (document.cpp),
/// navigation (navigation.cpp), editing (editing.cpp), clice's own requests
/// (extensions.cpp), pushing diagnostics and asking for refreshes
/// (push.cpp), progress (progress.cpp) and the test hooks (test_hooks.cpp).
class LSPClient {
public:
    /// Registers every handler on `peer` and connects to `server`, which
    /// talks to the client until it goes.
    LSPClient(MasterServer& server, ipc::JSONPeer& peer);
    ~LSPClient();

    // push.cpp

    /// Push a document's compile output to the client: its diagnostics — or,
    /// to a pulling client, a diagnostics refresh when an answer it holds
    /// went stale — plus the refreshes its arrival warrants. An output that
    /// changed with no compile (`compiled` false) pushes its diagnostics
    /// alone. Nothing until the handshake completed: the initialized handler
    /// replays what materialized before.
    void push_output(ServedProject& project, const Session& session, bool compiled);

    /// The document closed: its diagnostics are taken down, and what the
    /// client held of it is forgotten.
    void retract(Fid path_id);

    /// The one refresh: the documents' results changed, so the client asks
    /// again for the semantic tokens, inlay hints and folding ranges it
    /// holds, each refresh it takes. Document links and the outline have no
    /// refresh in LSP: the user reopens the file.
    void refresh();

    /// Ask a pulling client to pull every document's diagnostics again; a
    /// pull answered meanwhile is no report it holds any more.
    void refresh_diagnostics();

    /// Show a second name's diagnostics: the first name's while their texts
    /// agree, else one telling the user to close either. A pulling client
    /// gets them from its pulls, and is asked to pull again when the texts
    /// part or meet again.
    void publish_second_name(MasterServer::Document& document, MasterServer::SecondName& second);

    /// Push every served project's clice.toml problems as diagnostics on the
    /// configuration file's URI, and clear the ones of a configuration file
    /// no project reads any more: the projects served changed, or the client
    /// just became ready.
    void publish_settings_diagnostics();

    /// publish_second_name for every second name of `document`.
    void publish_second_names(MasterServer::Document& document);

    /// A second name closed or took the file over: its diagnostics end.
    void retract_second_name(llvm::StringRef spelling);

    /// The server must restart (MasterServer::restart_reasons): ask a client
    /// that restarts it, once; else tell the user, a configuration file by a
    /// diagnostic on it, the rest by a message.
    void ask_for_restart();

    /// Send every not-yet-forwarded notify-log message: guidance as
    /// window/showMessage, for the user to act on, an anomaly as
    /// window/logMessage.
    void forward_notify_messages();

    // progress.cpp

    /// Drive the background index's work-done progress through its
    /// begin/report/end lifecycle, reading MasterServer::index_progress.
    void report_index_progress();

    /// Follow a document's compile to `phase` with work-done progress (see
    /// CompileProgress); capability-gated.
    void report_compile_phase(Fid path_id, CompilePhase phase);

    /// The open documents whose semantic tokens or folds the client pulled
    /// from the index, or found empty while nothing served them: what the
    /// document's compile, or a background index merge, improves on. A
    /// commit of the document's compile clears it.
    llvm::DenseSet<Fid> index_served;

private:
    /// The last full or delta result sent for each open document, which
    /// the next delta request names.
    struct SentTokens {
        std::string result_id;
        std::vector<std::uint32_t> data;
    };

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

    using RawResult = async::task<codec::RawValue, ipc::Error>;
    using RequestContext = ipc::JSONPeer::RequestContext;

    // handshake.cpp
    void register_lifecycle();

    /// The error a request gets while the server's lifecycle refuses it:
    /// ServerNotInitialized before the handshake completed, InvalidRequest
    /// once shutdown was asked for. `shutdown` is answered as soon as
    /// initialize was.
    std::optional<ipc::Error> refused(bool shutdown = false) const;

    /// Registers the handler of a request behind the lifecycle check
    /// (refused): every request but initialize goes through one of these.
    template <typename Params, typename Handler>
    void on_request(Handler handler);
    template <typename Params, typename Handler>
    void on_request(std::string_view method, Handler handler);

    // sync.cpp
    void register_document_sync();

    // document.cpp
    void register_document_requests();

    /// Shared front half of every document-addressed handler: URI → path →
    /// interned path_id → open session (null when the document is not open)
    /// → the project serving it, held for the request: a folder removed
    /// while the request runs keeps its project alive until it completes.
    /// `version` is the document's as the request names it, a second name's
    /// own. The ticket is taken as the request is dispatched, ahead of the
    /// messages read with it: an edit among those arrives before the
    /// handler's task starts and supersedes it.
    struct ResolvedDoc {
        std::string path;
        Fid path_id;
        std::shared_ptr<Session> session;
        std::shared_ptr<ServedProject> project;
        std::optional<int> version;
        Ticket ticket;
    };

    ResolvedDoc resolve_uri(const std::string& uri);

    /// The second name a document spelled `spelling` is open as, or null.
    MasterServer::SecondName* second_name(llvm::StringRef spelling);

    /// Registers the handler of a request on a document: the user is
    /// active, the document resolves to its project, and `answer` — given
    /// the request's context, its parameters and the document — gets the
    /// request unless it needs the document open and it is not.
    template <typename Params, typename Answer>
    void on_document(Answer answer, DocumentRequestOptions options = {});

    /// What a semantic tokens request asks for: the whole document, the
    /// edits from the result the client holds, or the tokens in a range.
    struct TokensRequest {
        std::optional<std::string> previous;
        std::optional<protocol::Range> window;
    };

    RawResult send_tokens(ResolvedDoc document,
                          async::cancellation_token token,
                          TokensRequest request);

    // navigation.cpp
    void register_navigation();

    // editing.cpp
    void register_editing();

    // extensions.cpp
    void register_extensions();

    /// clice/switchContext, clice/resetContext and clice/switchConfiguration,
    /// which an editor asks for directly or through the commands of the
    /// source actions switching them (context_actions, configuration_actions).
    async::task<ext::SwitchContextResult> switch_context(const ext::SwitchContextParams& params);
    ext::SwitchContextResult reset_context(const ext::ResetContextParams& params);
    ext::SwitchConfigurationResult
        switch_configuration(const ext::SwitchConfigurationParams& params);

    /// The project serving the named document; without one the first
    /// project over a folder, the first one being rootless in a server
    /// without folders.
    std::shared_ptr<ServedProject> project_for(const std::optional<std::string>& uri);

    RawResult execute_command(protocol::ExecuteCommandParams params);

    // progress.cpp

    /// Wait out the delay, then announce `state`'s compile unless it ended.
    async::task<> announce_compile(std::shared_ptr<CompileProgress> state, Fid path_id);

    // test_hooks.cpp
    void register_test_hooks();

    // push.cpp

    /// The diagnostics shown on the file at `path` in the shape the client
    /// takes, `positions` mapping the text they were compiled from.
    std::vector<protocol::Diagnostic> to_client(llvm::ArrayRef<feature::FileDiagnostic> diagnostics,
                                                llvm::StringRef path,
                                                const PositionMap& positions);

    /// What a second name tells the user while its text differs from the
    /// first name's.
    std::string divergence_message(const MasterServer::SecondName& second);

    MasterServer& server;
    ipc::JSONPeer& peer;

    /// The client completed the initialized handshake. Until then the LSP
    /// spec forbids server→client traffic other than window/* messages, so
    /// diagnostics pushes and progress-token creation are held back;
    /// compile outputs missed in that window are replayed by the
    /// initialized handler from the documents' materialized output state.
    bool client_ready = false;

    /// Whether the client accepts workspace/semanticTokens/refresh,
    /// workspace/inlayHint/refresh and workspace/foldingRange/refresh (see
    /// refresh).
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

    /// How the client takes folds (textDocument.foldingRange).
    feature::FoldingClient folding_client;

    /// The legend of the semantic tokens, fixed when the client initializes.
    feature::SemanticTokenLegend token_legend;

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

    /// The client restarts the server when asked (clice/restartServer), as
    /// it declared under its experimental capabilities.
    bool restart_server = false;

    /// clice/restartServer was sent; the client is restarting the server.
    bool restart_asked = false;

    /// The restart reasons the user was told of in a message.
    llvm::StringSet<> restarts_told;

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

    llvm::DenseMap<Fid, Pulled> pulled;

    /// The configuration files publish_settings_diagnostics published last.
    llvm::StringSet<> published_settings_files;

    /// Subscription to the test hook's parked replies (clice/internal/held);
    /// disconnects on destruction.
    Signal<std::uint64_t>::Connection held_conn;

    /// Sequence number of the next notify-log message to forward (see
    /// MasterServer::notify_seq).
    std::uint64_t notify_cursor = 0;

    /// The guidance shown to the user, each text once.
    llvm::StringSet<> shown_guidance;

    std::shared_ptr<IndexProgressState> index_progress = std::make_shared<IndexProgressState>();

    /// The documents whose compile runs or whose announcement is pending.
    llvm::DenseMap<Fid, std::shared_ptr<CompileProgress>> compile_progress;
};

/// The commands of the source actions switching what a document compiles
/// under (context_actions, configuration_actions), each standing for the
/// request of the same name with its parameters as the one argument.
/// Named apart from the VS Code extension's commands, which the client
/// library registers them beside.
constexpr inline std::string_view switch_context_command = "clice.lsp.switchContext";
constexpr inline std::string_view reset_context_command = "clice.lsp.resetContext";
constexpr inline std::string_view switch_configuration_command = "clice.lsp.switchConfiguration";

/// The kinds of those source actions.
constexpr inline std::string_view context_action_kind = "source.clice.context";
constexpr inline std::string_view configuration_action_kind = "source.clice.configuration";

/// The build configurations a project's settings declare, the one active
/// and the one selected for the next start (clice/listConfigurations).
ext::ListConfigurationsResult list_configurations(const Project& project);

/// The file a `file:` URI names; nullopt for any other URI.
std::optional<Spelling> uri_to_path(llvm::StringRef uri);

/// The answer to a diagnostics pull.
codec::RawValue full_report(std::vector<protocol::Diagnostic> items);

template <typename Params, typename Handler>
void LSPClient::on_request(Handler handler) {
    using Result = std::invoke_result_t<Handler&, RequestContext&, const Params&>;
    peer.on_request(
        [this, handler = std::move(handler)](RequestContext& ctx, const Params& params) -> Result {
            if(auto error = refused(std::is_same_v<Params, protocol::ShutdownParams>)) {
                return kotatsu::outcome_error(std::move(*error));
            }
            return handler(ctx, params);
        });
}

template <typename Params, typename Handler>
void LSPClient::on_request(std::string_view method, Handler handler) {
    using Result = std::invoke_result_t<Handler&, RequestContext&, const Params&>;
    peer.on_request(
        method,
        [this, handler = std::move(handler)](RequestContext& ctx, const Params& params) -> Result {
            if(auto error = refused()) {
                return kotatsu::outcome_error(std::move(*error));
            }
            return handler(ctx, params);
        });
}

template <typename Params, typename Answer>
void LSPClient::on_document(Answer answer, DocumentRequestOptions options) {
    on_request<Params>([this,
                        answer = std::move(answer),
                        options](RequestContext& ctx, const Params& params) -> RawResult {
        server.pool.foreground_pulse();
        auto document = [&] {
            if constexpr(requires { params.item; }) {
                return resolve_uri(params.item.uri);
            } else {
                return resolve_uri(params.text_document.uri);
            }
        }();
        if(!options.closed && !document.session) {
            // A second name whose text parted from the first name's has no
            // answers until they agree again.
            if(second_name(document.path)) {
                return kotatsu::outcome_error(content_modified());
            }
            return kotatsu::outcome_error(
                ipc::Error{protocol::ErrorCode::InvalidParams, "Document not open"});
        }
        // A project whose settings have errors serves nothing under its root.
        if(document.project->broken()) {
            if constexpr(std::same_as<Params, protocol::DocumentDiagnosticParams>) {
                return async::co_invoke([]() -> RawResult {
                    co_return full_report(std::vector<protocol::Diagnostic>());
                });
            } else {
                return async::co_invoke([]() -> RawResult { co_return codec::RawValue{"null"}; });
            }
        }
        return answer(ctx, params, std::move(document));
    });
}

/// What `navigate`'s answer gives: the reply, or the error to answer with.
using NavigationReply = std::expected<codec::RawValue, ipc::Error>;

/// Answer with what a gate decided instead (GateStop).
async::task<codec::RawValue, ipc::Error> stop_reply(GateStop stop);

/// A navigation request's answer from the symbol at its position,
/// `answer`'s, once the compile gate passed (ServedProject::navigation_gate);
/// `none` where no symbol is. A document not open is answered from the
/// index alone.
template <typename Answer>
async::task<codec::RawValue, ipc::Error> navigate(ServedProject& project,
                                                  const Ticket& ticket,
                                                  Fid path_id,
                                                  const protocol::Position& position,
                                                  llvm::StringRef none,
                                                  Answer answer) {
    if(ticket.session) {
        if(auto stop = co_await project.navigation_gate(ticket)) {
            co_return co_await stop_reply(std::move(*stop));
        }
    }
    auto cursor = project.cursor_at(path_id, position);
    if(!cursor) {
        co_return codec::RawValue{none.str()};
    }
    auto reply = answer(*cursor);
    if(!reply) {
        co_await async::fail(std::move(reply.error()));
    }
    co_return std::move(*reply);
}

}  // namespace clice

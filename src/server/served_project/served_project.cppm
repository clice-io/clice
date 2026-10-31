module clice:server.served_project;

import :command.database;
import :command.search_config;
import :feature.code_completion;
import :feature.diagnostics;
import :index.peer_queries;
import :index.query;
import :index.shard;
import :index.tu_index;
import :project.command_resolver;
import :project.hosting;
import :project.project;
import :sched.builders.pch;
import :sched.builders.pcm;
import :sched.graph;
import :sched.stack;
import :server.session;
import :settings.settings;
import :support.anomaly;
import :support.logging;
import :support.prelude;
import :support.signal;
import :support.timer;
import :syntax.position;
import :syntax.source_text;
import :vfs.disk_state;
import :vfs.file_system;
import :vfs.file_table;
import :worker.pool;
import :worker.protocol;

namespace clice {

class MasterServer;

namespace testing {

struct ServedProjectFixture;

}

/// What a document's compile round waits on.
enum class CompilePhase : std::uint8_t {
    /// No round runs.
    Idle,
    /// The command it compiles with.
    Command,
    /// The modules it imports.
    Modules,
    /// Its preamble.
    Preamble,
    /// A worker parsing it.
    Parsing,
};

/// An open document's resource-investment state. Written at exactly two
/// points: adoption (from the readonly mode) and ServedProject::escalate (the
/// triggers). Everything else derives routing from readiness, not from this
/// flag.
enum class ServingMode : std::uint8_t {
    /// No PCH/AST investment: the document is served from the index.
    IndexOnly,
    /// PCH/AST investment is on; the index still answers while a compile
    /// is in flight.
    Escalated,
};

/// The publishable products of the most recent compilation (materialized
/// whole-document feature results). The data lives in the document's
/// compile; the client reads it from there when it is pushed, and with no
/// client connected it simply stays put.
struct CompileOutput {
    /// Document version the compile ran against; empty on the clear path
    /// (a failed compile publishes empty diagnostics without a version).
    std::optional<int> version;

    /// The text compiled, which the diagnostics are at bytes of.
    std::shared_ptr<SourceText> text;

    /// What the compile raised and what clice knows of how it compiled.
    feature::CompileDiagnostics diagnostics;

    /// The compile failed before parsing: its diagnostics say why, and
    /// nothing else came of it.
    bool failed = false;
};

/// One open document's compilation products, read by everything that
/// answers from the document's compile. Immutable once published: every
/// change installs a fresh one (copy-on-write over the cheap members), so a
/// reader that copied the shared_ptr may keep using it across suspension
/// points.
struct DocumentCompile {
    /// Content key into Project.pch_cache for the PCH the document's parse
    /// loads, if any: its preamble's, else the precompiled header the build
    /// declares for its command — or the preamble's whose build failed
    /// (pch_failed). The PCH itself is owned by Project (shared,
    /// content-addressed); whether its preamble-derived state still
    /// describes the buffer is checked against the blob's stored preamble
    /// text at the point of use.
    std::optional<std::string> pch_key;

    /// The build of pch_key failed, and the parse loaded the declared one
    /// alone, if any: what a save of the document retries.
    bool pch_failed = false;

    /// Content key into Project.pch_cache for the precompiled header the
    /// build declares for the document's command, when the parse loads it:
    /// under its preamble's PCH, or as pch_key itself.
    std::optional<std::string> base_pch_key;

    /// The host whose command the compile borrowed; invalid for none.
    Fid host;

    /// The key of the PCH the parse loaded.
    const std::optional<std::string>& loaded_pch() const {
        return pch_failed ? base_pch_key : pch_key;
    }

    /// The latest compilation's TU index; null until a compile commits
    /// index data. NOT merged into Project.index — that only gets
    /// disk-derived data from background indexing.
    std::shared_ptr<index::TUIndex> index;

    /// Publishable products of the latest compilation (see CompileOutput).
    std::optional<CompileOutput> output;

    /// What the latest compilation instantiates the file's templates with,
    /// for the document's completions.
    feature::InstantiatedTemplates instantiated;
};

/// An open document as the project serving it sees it: its latest compile
/// and whether that describes the buffer, whether the document is compiled
/// or served from the index, and whether its command was borrowed or
/// guessed.
struct ServedDocument {
    std::shared_ptr<Session> session;

    /// Null before the document's first compile.
    std::shared_ptr<const DocumentCompile> latest;

    /// Dependency snapshot from the last successful compile, used for
    /// staleness detection.
    std::optional<DepsSnapshot> deps;

    /// Whether the latest compile describes the current buffer: the last
    /// compile finished and no invalidation arrived since. Written by the
    /// round's commit and by every invalidation.
    bool current = false;

    /// Bumped by every invalidation of the document. A stateless build
    /// snapshots it before its PCH acquisition and may adopt the key only
    /// if it is unchanged, beside the buffer's generation (a Lost-type
    /// invalidation means the resolved command may describe nothing).
    std::uint64_t epoch = 0;

    /// See ServingMode for the write discipline.
    ServingMode serving = ServingMode::Escalated;

    /// The last resolution of the document's command borrowed or
    /// synthesized it (Inferred, Fallback). A database change may give it a
    /// real command or change its lender's, which the change's own delta
    /// cannot tell: every change recompiles it.
    bool guessed = false;
};

/// LSP's ContentModified: the request's answer would describe a buffer the
/// client already moved past. Clients keep what they have and re-pull —
/// unlike a null, which a client is entitled to read as "there is nothing"
/// (VS Code clears semantic highlighting on one).
ipc::Error content_modified();

/// What answers a request the index may serve, derived again from the
/// document's state each time (ServedProject::pick_route).
enum class Route : std::uint8_t {
    /// The ticket went stale (an edit, a move, the close): answer
    /// ContentModified — the client re-requests against the new state.
    Outdated,
    /// Serve from the index source the freshness contract admits.
    Index,
    /// Ask the worker holding the AST (ServedProject::ast_request, which
    /// compiles as needed).
    Ast,
    /// No source can serve and none is being invested in (IndexOnly with
    /// no matching shard): answer honestly empty.
    Empty,
};

struct RouteOptions {
    /// The feature's answer from the index raw-lexes the whole buffer
    /// (semantic tokens, folds, the outline): it follows the investment
    /// policy once the buffer is oversized, while answers that only look
    /// the index up serve at any size.
    bool full_lex = false;

    /// The feature has no refresh request, so an empty reply to a cold
    /// document would be cached by the client for good — no signal ever
    /// makes it re-pull. Await the didOpen boost once (bounded by one index
    /// attempt) before answering Empty, and decide again from whatever
    /// source is there once it finishes: the shard, the escalated compile,
    /// or honestly empty under readonly "on".
    bool await_cold_attempt = false;
};

/// What a gate decided instead of the feature's own answer: null (no
/// source can answer — a failed compile) or ContentModified.
struct GateStop {
    std::optional<ipc::Error> error;
};

/// The largest buffer the master lexes whole on its own thread.
constexpr std::size_t full_lex_cap = 8 * 1024 * 1024;

/// What a request answers when the workers could not serve it.
template <typename Value>
Value empty_value() {
    if constexpr(std::is_same_v<Value, codec::RawValue>) {
        return codec::RawValue{"null"};
    } else {
        return Value{};
    }
}

/// One project the server serves: the project on disk and its scheduling
/// stack over the process's worker pool, the open documents the master
/// routed to it, and everything the project derives for them. Its work is
/// split in files: lifetime and adoption (served_project.cpp), compiling the
/// open documents (compile.cpp), what answers a request on them now and
/// asking the workers (answer.cpp), handling changes of its inputs
/// (changes.cpp), the open documents' commands (command.cpp) and the index's
/// live sources over them (live_sources.cpp). Shared: a request in flight
/// keeps a removed project alive until it completes.
class ServedProject final :
    public index::LiveSources,
    public std::enable_shared_from_this<ServedProject> {
public:
    /// `root` is the project's directory; empty for the rootless project a
    /// server without folders runs, which loads nothing.
    ServedProject(MasterServer& server, CanonicalPath root);
    ~ServedProject();

    // served_project.cpp

    /// Read the settings (load_settings): clice.toml under the root,
    /// overlaid with the client's initializationOptions (`init_options`),
    /// its cache directory none of `taken_cache_dirs`. One with errors
    /// leaves the project broken.
    void configure(const std::optional<codec::dyn::Value>& init_options,
                   llvm::ArrayRef<CanonicalPath> taken_cache_dirs);

    /// Open the project (Project::open), queue what its index lacks, load
    /// the context choices it saved, and start its store-lifetime services;
    /// then watch its databases and files. Documents adopted before have
    /// their serving decided now. A broken project starts nothing.
    void start();

    /// Quiesce compile and index work and persist the final state; then
    /// sched.close() releases the store — once the pool stopped at exit, or
    /// right away for a project removed while the pool serves others (a
    /// worker still finishing a cancelled build of it only fails its
    /// write).
    async::task<> shutdown();

    /// The settings have errors: the project starts nothing, and the
    /// files under its root answer every request at once with nothing.
    bool broken() const {
        return has_error(settings_issues);
    }

    /// Take over an open document the master routed here — just opened, or
    /// moving from another project: discover the databases around it, and
    /// decide whether it is compiled or served from the index (now, or once
    /// the project starts).
    void adopt(std::shared_ptr<Session> session);

    /// Let go of a document closed or moving to another project: the worker
    /// holding its AST drops it, its compile goes, and its background index
    /// is queued again, which its compile stood in for.
    void release(Fid path_id);

    /// The document open here, or null when it is not this project's.
    ServedDocument* find_document(Fid path_id);
    const ServedDocument* find_document(Fid path_id) const;

    /// Whether `session` is a document of this project: the project writes
    /// about a document — its output, a crash, a refresh — only while it is.
    bool owns(const Session& session) const;

    /// After the editor saved a file routed here: record a source of the
    /// project the build does not declare as a provisional member
    /// (Build::record), the build following it as a database reload would.
    void record_saved(Fid path_id);

    // compile.cpp

    /// Compile an open document's AST if it is not current: join the
    /// document's round (spawning one if none is live) and retry through
    /// stale attempts until a terminal outcome. Returns true when the
    /// latest compile is current on return. A new generation of the
    /// document abandons the join — the editor re-requests after an edit,
    /// so the result would describe a buffer that no longer exists.
    async::task<bool> ensure_compiled(std::shared_ptr<Session> session);

    /// Start (or join) the document's compile detached — the caller keeps
    /// serving from the index until it finishes. The graph's
    /// one-round-per-node discipline dedupes concurrent requests.
    void request_compile(std::shared_ptr<Session> session);

    /// The escalation triggers' single write point: flip an index-only
    /// document to Escalated. The build stays pull-driven — the next
    /// request that needs the AST starts it. A no-op for already-escalated
    /// documents and under readonly = "on" (the mode transition is
    /// disabled).
    void escalate(Fid path_id);

    /// The document was edited: the latest compile is no longer current and
    /// the in-flight round's result is void. Fires the round's advisory
    /// token and cancels its compile over IPC, which interrupts the
    /// worker's parse (the round still observes the real reply: crash
    /// accounting depends on it); the document is compiled from now on
    /// (escalate).
    void edited(Session& session);

    /// The document compiles in another context now: a different
    /// compilation identity. Supersede any in-flight compile and drop the
    /// state earned under the old one — including the self-containment
    /// trial, since a different host can change the macro environment.
    void choice_changed(Session& session);

    /// A Lost-type invalidation (dependency changed on disk, worker crash,
    /// eviction — the buffer itself is unchanged): the latest compile is no
    /// longer current and an in-flight round must not commit as such, but
    /// its parse keeps running — the round publishes the product as bounded
    /// staleness before reporting Stale.
    void invalidate(Fid path_id);

    /// didSave: every crashed kind of the document retries on its next
    /// request, the artifacts it consumes included, and so do the ones
    /// whose build failed; a header found self-contained whose compile
    /// misses names, or has not finished, re-runs its self-containment
    /// trial.
    void retry_after_save(Session& session);

    /// The inputs of a stateless build carrying the document's buffer:
    /// module dependencies built or revalidated, the preamble PCH built or
    /// reused, and the PCM path table. Under readonly = "on" the PCH step
    /// is skipped — the build compiles without a preamble, the profile's
    /// stated trade.
    struct StatelessInputs {
        std::pair<std::string, std::uint32_t> pch;
        std::unordered_map<std::string, std::string> pcms;
    };

    /// The PCH key is adopted only while `ticket` and the document's epoch
    /// snapshotted here, before the first suspension, hold: a supersede
    /// bumps the generation, a Lost-type invalidation bumps only the epoch
    /// — either way the resolved command may describe a command that no
    /// longer exists, and adopting the key would hand the document's
    /// preamble features and staleness checks a PCH of another command.
    /// This path runs no graph round it could ask instead. The inputs
    /// themselves follow `text`, the buffer the build sends, so a request
    /// that outlives its ticket still compiles against a PCH of its own
    /// preamble. False only when the graph shut down: an import that did
    /// not build is the compile's to report.
    async::task<bool> prepare_stateless_inputs(const Ticket& ticket,
                                               llvm::StringRef text,
                                               std::uint32_t point,
                                               const std::string& directory,
                                               const std::vector<std::string>& arguments,
                                               const Resolution& resolution,
                                               StatelessInputs& inputs);

    /// What the document's compile read, one snapshot at a time: its own
    /// deps, its header context's chain, its PCH's and those of the modules
    /// it imports — of a module that failed to build, what the failure
    /// rests on (Project::build_inputs). Both the staleness check and the
    /// change handling's readers go through it: what a request finds
    /// changed and what a disk change reaches are the same files.
    void each_input(Fid path_id, llvm::function_ref<void(const DepsSnapshot&)> visit) const;

    /// A dispatch of `kind` killed a worker on the document's content:
    /// `death` is the death's identity (worker::death_of), `cause` how it
    /// died. Records it in the document's crash records and republishes,
    /// so the crash note shows.
    void record_crash(const std::shared_ptr<Session>& session,
                      feature::RequestKind kind,
                      llvm::StringRef death,
                      std::string cause);

    /// Push the document's output again: its crash notes changed with no
    /// compile to carry them. A document that never compiled gets an empty
    /// versionless output to hang them on.
    void republish(const std::shared_ptr<Session>& session);

    /// The compile, or the preamble it consumes, crashed a worker and may
    /// not retry now: the document has no AST to offer.
    bool compile_stopped(const Session& session) const;

    /// Whether a round of the document is live: it will send a compile of
    /// its own, so a lost worker-side AST needs no invalidation.
    bool compiling(Fid path_id) const {
        return sched.graph.is_compiling(node(path_id));
    }

    /// The document's latest compile, or null before its first compile.
    std::shared_ptr<const DocumentCompile> latest(Fid path_id) const;

    /// The document's latest compile while its output describes buffer
    /// `version`, or null.
    std::shared_ptr<const DocumentCompile> latest_at(Fid path_id, int version) const;

    /// The last compile finished and no invalidation arrived since.
    bool current(Fid path_id) const;

    /// Whether the latest compile's index describes the current buffer.
    /// The arbitration key of the index freshness contract (IndexQuery
    /// clauses 3-4): a current-but-indexless compile (no AST, or an index
    /// the worker could not hand over) is an honest gap, not a servable
    /// index.
    bool index_current(Fid path_id) const;

    /// What the document's diagnostics show of a compile output of its:
    /// the output's diagnostics (feature::shown_diagnostics), then the
    /// crash notes. The one assembly of the pull and of the push, which
    /// keep their own rules of which output and which version.
    std::vector<feature::FileDiagnostic> shown_diagnostics(const Session& session,
                                                           const CompileOutput& output) const;

    /// What the diagnostics pull answers once the compile finished, without
    /// waiting for one: the output's diagnostics while it describes the
    /// buffer, then the crash notes; nothing once the document closed.
    std::vector<feature::FileDiagnostic> finished_diagnostics(const Session& session) const;

    /// Whether the file's escalated document holds text no background run
    /// has reported on, as after a restart, when the index is current and
    /// runs nothing. Only a file of its own database entry is reported on
    /// (TURunFamily::Plan::skipped_diagnostics).
    bool lacks_skipped_diagnostics(Fid path_id) const;

    /// Interrupt every in-flight parse and wait for the detached compile
    /// joins. The rounds themselves finish in TaskGraph::shutdown — the
    /// interruption is what keeps that finish prompt (a round's stateful
    /// send carries the interrupt, not its advisory token: contract 2 wants
    /// the real reply, and the interrupt makes it arrive early).
    async::task<> stop_compiles();

    // command.cpp

    /// The open document's command under its active context choice, or
    /// under `candidate` instead, recording whether the command was borrowed
    /// or guessed (but for a candidate's). Takes no include tree: the host
    /// search runs on the trees there are.
    Resolution resolve_command(Fid path_id,
                               std::string& directory,
                               std::vector<std::string>& arguments,
                               const ContextChoice* candidate = nullptr);

    /// resolve_command again until the host search it runs misses no
    /// include tree, taking each tree it misses first from a worker, each
    /// once: a header compiles under the host every process finds for it.
    /// Stops with the tree missing when a run does not answer, or once
    /// `current` no longer holds.
    async::task<Resolution> document_command(Fid path_id,
                                             std::string& directory,
                                             std::vector<std::string>& arguments,
                                             const ContextChoice* candidate = nullptr,
                                             llvm::function_ref<bool()> current = {});

    /// The file's context choice while it is this project's and holds.
    const ContextChoice* active_choice(Fid path_id) const;

    /// The host source of a header: its active choice's, else
    /// clice::host_of.
    Fid host_of(Fid path_id);

    /// The dialect the server lexes an open file's text in, for the results
    /// computed from its index, hover's directive lookup and completion's
    /// context checks: clice::dialect under the file's active choice and
    /// host.
    const clang::LangOptions& dialect(Fid path_id);

    // changes.cpp

    /// Files the project knows changed on disk (MasterServer::
    /// apply_disk_changes): rescan their include edges and module
    /// declarations, recompile the open documents reading them, queue the
    /// closed units reading them, invalidate the PCMs built from them. A
    /// file seen missing is removed.
    void disk_changed(llvm::ArrayRef<Fid> files);

    /// Reload what the looks found changed of the project's databases
    /// (`force` skips the content gate and the two-tick debounce, the
    /// half-written-file guard) and follow the reload; how many reloads
    /// changed the build. None when the databases moved to another build's
    /// index (databases_moved): only a new server opens that one, which is
    /// asked for (MasterServer::ask_restart).
    std::size_t reload_databases(bool force);

    /// A file created under a default-command rule joins the build, as a
    /// database reload adds a command: whether one did. One deleted leaves
    /// through disk_changed, like any file. The walk runs off the event
    /// loop.
    async::task<bool> poll_sources();

    /// Whether the build's databases resolve elsewhere than at the load
    /// (resolved_databases): another build, whose commands a reload would
    /// record as the ones this index was built with.
    bool databases_moved() const;

    /// Before a file's first compile: register, load and watch the
    /// databases in the directories from the file's up to the root, which
    /// startup discovery (the root and its direct subdirectories) did not
    /// look at — a file of a deeper project compiles from its own database —
    /// and follow the loads. Nothing when a rule declares sources, the file
    /// has a command already, or it lies outside the root.
    void discover_around(Fid path_id);

    /// The ASTs a worker held of these documents are gone — it crashed, or
    /// evicted one: they compile again, with nothing of their inputs
    /// changed.
    void asts_lost(llvm::ArrayRef<Fid> documents);

    /// Every unit whose compile or build depends on the file — the single
    /// answer to "who read this": the root TUs the lexical scan sees
    /// including it, the TUs whose index records reading, looking for or
    /// importing a module built from it, the open documents whose inputs
    /// (each_input) name it, the module units whose last PCM build read it,
    /// failed builds included — the scan cannot resolve a macro include,
    /// never follows an import, and never sees a file before it exists.
    llvm::SmallVector<Fid> readers(Fid path_id) const;

    /// Whether anything here derives from the file: an open document, a
    /// command, an include edge, index entries, a compile that read it or
    /// looked for it. The disk changes of files it does not know are not
    /// this project's to cascade.
    /// FIXME: a rule with a default command and no patterns gives every file a
    /// command, so such a project takes every other project's changes too.
    bool knows(Fid path_id);

    /// The files every open document's compile depends on (closure).
    void append_open_inputs(llvm::SmallVectorImpl<Fid>& files) const;

    /// Whether an open document's own compile serves the file's index
    /// (IndexPump::compiled_by_session): an escalated document's does,
    /// unless it skips the bodies of the headers — then only a background
    /// run shows what the skipped bodies hold (report_skipped_diagnostics).
    bool compiled_by_session(Fid path_id) const;

    /// The project's context choices hold again or no longer do after the
    /// build or the include graph changed: an open document whose choice
    /// flipped compiles in another context, and routing follows.
    void recheck_choices();

    // answer.cpp: what answers a request on an open document now, and
    // asking the workers for it. An answer comes from the worker holding
    // the document's AST, from the products of its PCH build (the
    // preamble's links, cached here because the AST is built on top of the
    // PCH and cannot see the preamble), or from the index; which one is
    // derived per request from readiness, never from a mode flag. Every
    // dispatch of a document's content to a worker is admitted by its crash
    // records and finishes through one exit (finish).

    /// How the project serves the document of a ticket still fresh: a move
    /// or a close supersedes every ticket of the document.
    ServingMode serving_of(const Session& session) const;

    /// Whether the worker's AST can answer for this document right now:
    /// compiled, current, and its compile not stopped by a crash (the check
    /// sits before ensure_compiled's clean-AST fast path, so a stopped
    /// document's query returns null even with a clean AST). When false,
    /// the routing tries the index before deciding to await a compile.
    bool ast_answerable(const Session& session) const;

    /// See Route: a current AST answers; otherwise an index source that is
    /// byte-identical to the buffer; otherwise the compile the policy owes,
    /// or nothing when it owes none. Starts an escalated document's compile
    /// so an index answer never strands the AST investment the policy asked
    /// for. A Route::Index decision stores the admitted source into `source`
    /// (when given) — callers must serve from it rather than re-derive, or
    /// the state could shift between the decision and the read.
    async::task<Route> pick_route(const Ticket& ticket,
                                  RouteOptions options,
                                  std::optional<index::IndexSource>* source = nullptr);

    /// The compile gate of index-navigation requests: awaits the compile
    /// exactly when the routing decided the AST is the serving source
    /// (freshness clause 1 needs the finished file index), and lets
    /// index-served documents resolve through clauses 1/4 without forcing
    /// the build. A GateStop means answer with it instead: the compile
    /// failed, or the request was superseded.
    async::task<std::optional<GateStop>> navigation_gate(const Ticket& ticket);

    /// Await the document's compile; a GateStop when it failed or the
    /// request was superseded.
    async::task<std::optional<GateStop>> await_compile(const Ticket& ticket);

    /// The symbol under a position of the file's serving source, if any.
    std::optional<index::IndexQuery::Cursor> cursor_at(Fid path_id,
                                                       const protocol::Position& position) const;

    /// The project's query and its peers', for one synchronous read: the
    /// peers are those the master serves when it is taken.
    index::PeerQueries peer_queries();

    /// The document's text, read in the dialect the server lexes it in.
    SourceText& text_of(Session& session);

    /// The header search of the command the open file compiles under.
    SearchConfig search_of(Fid path_id);

    /// The preamble include links of a document's active PCH; empty when
    /// there is no PCH or its preamble no longer matches the buffer.
    std::vector<index::DocumentLink> preamble_links(const Session& session) const;

    /// The include-like directive links of a document's whole buffer: the
    /// PCH's cached preamble links, then the worker's AST's for the rest.
    /// Module names go through the ordinary index pipeline, not these.
    async::task<std::vector<index::DocumentLink>, ipc::Error>
        directive_links(const Ticket& ticket, async::cancellation_token token);

    /// Whether a dispatch of `kind` may reach a worker: its record does not
    /// stop it — or, once an attempt holds it, `kind` did not crash during
    /// its flight — and for a build that parses the document (`parses`),
    /// neither does the compile's or its preamble's. One check admits a
    /// dispatch, of every kind: before its inputs are prepared, and again
    /// where it reaches a worker — after a stateless request's wait in the
    /// queue and before every resend — since a request like it can crash on
    /// the same content meanwhile.
    bool admits(const Session& session,
                feature::RequestKind kind,
                const Session::Attempt* attempt,
                bool parses) const;

    /// Ask the worker holding the document's AST: compile it if it is not
    /// current, check its crash records, send, and finish; a missing AST
    /// answers an empty result. `params` comes with the fields particular to
    /// its request filled in, its path set here. `token` is the request's:
    /// passing it into the worker send turns a client $/cancelRequest into
    /// an IPC cancel — the worker stops the parse at the next top-level
    /// declaration instead of computing a result nobody will read. The
    /// shared compile a query waits on is deliberately NOT cancelled: it
    /// serves every waiter, not this request.
    template <typename Params>
    async::task<typename protocol::RequestTraits<Params>::Result, ipc::Error>
        ast_request(const Ticket& ticket,
                    feature::RequestKind kind,
                    llvm::StringRef label,
                    Params params,
                    async::cancellation_token token);

    /// The interactive stateless builds (completion, signature help): the
    /// buffer and its compile inputs go to a stateless worker, which
    /// compiles a buffer state the shared AST round may not have seen yet,
    /// at `offset` of it. `params` arrives with the fields particular to its
    /// request filled in.
    template <typename Params>
    async::task<typename protocol::RequestTraits<Params>::Result, ipc::Error>
        stateless_request(const Ticket& ticket,
                          feature::RequestKind kind,
                          llvm::StringRef label,
                          std::uint32_t offset,
                          Params params,
                          async::cancellation_token token);

    /// Format the ticket's buffer, or a range of it, on a stateless worker.
    async::task<std::optional<worker::FormatResult>, ipc::Error>
        format(const Ticket& ticket,
               std::optional<protocol::Range> range,
               async::cancellation_token token);

    /// The single exit of every dispatch: a fresh reply clears the kind's
    /// crash record, a stale one never leaves as a value — unless it is a
    /// `snapshot` reply, which describes the buffer the request carried and
    /// which the client reconciles with the edits made meanwhile. A fresh
    /// dispatch the workers could not serve answers empty: the crash note on
    /// the document says why, and an error would only show as a client
    /// popup.
    template <typename Outcome>
    Outcome finish(const Ticket& ticket,
                   feature::RequestKind kind,
                   llvm::StringRef label,
                   Outcome result,
                   bool snapshot = false);

    // live_sources.cpp: the open documents' own file indexes (current per
    // their latest compiles), the PCH overlays their preambles were
    // compiled into, and the preamble regions those overlays indexed.

    bool is_open(Fid file) const override;
    std::optional<index::IndexSource> buffer_index(Fid file) const override;
    void for_each_session(llvm::function_ref<bool(const index::IndexSource&)> visit) const override;
    void for_each_session_index(
        llvm::function_ref<bool(const index::TUIndex&)> visit) const override;
    void
        for_each_preamble(llvm::function_ref<bool(const index::IndexSource&)> visit) const override;
    void for_each_overlay(llvm::function_ref<bool(const index::TUIndex&)> visit) const override;
    std::shared_ptr<index::TUIndex> preamble_index(Fid file) const override;
    void for_each_pch_index(Fid file,
                            llvm::function_ref<void(const index::TUIndex&)> visit) const override;

    /// The project's shard of the file while the document's buffer holds
    /// the very bytes it indexed.
    const index::Shard* matching_shard(const Session& session) const;

    /// Set while compiles may run and requests resolve commands. A project
    /// made while its root's previous one still shuts down
    /// (MasterServer::serve_folders) holds documents before it loads, which
    /// would compile under no build at all; stop_compiles() lets them go
    /// too.
    async::event loaded{true};

    MasterServer& server;

    Project project;

    CommandResolver commands{project};

    /// The scheduling core the batch driver runs too, its builders
    /// registered at construction — nodes materialize on demand, so a
    /// module-free project pays nothing. The pump is serving-neutral; the
    /// document-side policy is installed into its hooks at construction.
    SchedulingStack sched;

    /// Problems found while reading the settings, kept so the client can
    /// publish them as diagnostics on the configuration file's URI, and the
    /// path of the file found (empty when none).
    std::vector<SettingsIssue> settings_issues;
    std::string settings_file;

private:
    friend struct testing::ServedProjectFixture;

    static NodeId node(Fid path_id) {
        return {Family::AST, path_id.raw};
    }

    // served_project.cpp

    /// Decide a freshly opened index-only document's serving: escalate one
    /// that already diverged from its shard, or boost the file's background
    /// indexing when nothing can serve it. Escalated documents need no
    /// decision — builds are pull-driven.
    void decide_serving(ServedDocument& document);

    /// When this project holds its cache directory's writer lock, the
    /// commands that find it taken ask this server to index for them:
    /// listen on a loopback port and record it next to the lock (see
    /// index/writer_lock.cppm). The record is removed at shutdown.
    void start_control_listener();

    /// Schedule a save carrying dirty artifact/context metadata; no-op
    /// when one is already scheduled or the server is shutting down (the
    /// final shutdown save covers it).
    void schedule_metadata_flush();
    async::task<> metadata_flush_task();

    /// The periodic SchedulingStack::checkpoint().
    async::task<> checkpoint_task();

    /// Drop pch_cache metadata for blobs the store's LRU evicted from
    /// disk (see checkpoint_task).
    void forget_evicted_pchs();

    /// The default-sources loop, poll_sources on the workspace poll's pace.
    /// Spawned by start() unless polling is off.
    async::task<> sources_poll_task();

    // compile.cpp

    /// A file the open document reads changed: its compile, its
    /// self-containment trial and its stopped request kinds start over.
    void inputs_changed(Session& session);

    /// One compile round; see run_round's comments for its obligations.
    async::task<RoundOutcome> run_round(RoundContext& ctx, Fid path_id);

    /// The module-dependency phase of a round: resolve imports from the
    /// round's buffer snapshot under the round's resolved command,
    /// revalidate on-disk PCM blobs, declare the Ast→PCM durable edges
    /// (scanner truth — they must survive a failed compile or fixing an
    /// import could never re-dirty this document), and wait on each
    /// import through depend. Returns the imports, none when cancelled: an
    /// import whose build failed is left to the parse, which reports it on
    /// the import next to the file's own diagnostics; one whose build
    /// crashed a worker also goes into the document's records, and is
    /// refused until the document's record lets it retry.
    async::task<std::optional<PCMBuilder::ModuleDeps>>
        depend_modules(RoundContext& ctx,
                       const Ticket& ticket,
                       const Resolution& resolution,
                       llvm::StringRef directory,
                       const std::vector<std::string>& arguments,
                       llvm::StringRef text);

    /// Whose fault a worker death under the document's compile is, tried
    /// in turn: the clang-tidy pass, when `tidy` ran it, the PCH pair
    /// `consumed` the parse read, else the document's own code. Returns
    /// whether the compile retries without the suspect: the pass left out,
    /// the pair retracted and rebuilt.
    bool attribute_crash(const std::shared_ptr<Session>& session,
                         bool tidy,
                         const std::optional<std::string>& consumed,
                         const ipc::Error& error);

    /// What a buffer state owes the PCH builder: nothing (an empty preamble
    /// with no injected prefix, or a module unit's global module fragment
    /// that imports — a previously adopted key must be cleared, unless the
    /// plan is partial), or the acquisition of `request`.
    struct PCHPlan {
        enum class Decision : std::uint8_t { None, Acquire };

        Decision decision = Decision::None;

        PCHBuilder::Request request;

        /// The preamble ends before the text's own, at a completion's point:
        /// what the request loads is its own, not the document's.
        bool partial = false;
    };

    /// The request for the precompiled header the build declares for the
    /// command `resolution` gave the document: the editor's build, skipping
    /// function bodies, but for a modular parse (ModuleDeps::modular), which
    /// reads the index's, the one every PCM is built over.
    std::optional<PCHBuilder::Request> declared_pch(Fid path_id,
                                                    const Resolution& resolution,
                                                    const PCMBuilder::ModuleDeps& imports);

    /// `base` is the key of the precompiled header the build declares for
    /// the command, built already; empty for none. `point` is where a
    /// completion's parse stops: the preamble ends before the line holding
    /// it, so the lines typed there keep the PCH of the lines above them.
    PCHPlan plan_pch(Fid path_id,
                     llvm::StringRef text,
                     std::optional<std::uint32_t> point,
                     const std::string& directory,
                     const std::vector<std::string>& arguments,
                     const Resolution& resolution,
                     const PCMBuilder::ModuleDeps& imports,
                     llvm::StringRef base);

    /// Revalidate or build the preamble PCH of `text` through the builder,
    /// over the precompiled header the build declares for the command, and
    /// adopt the key of what the request loads while the ticket and the
    /// document's `epoch` hold (see prepare_stateless_inputs), but for a
    /// partial plan's. Returns the key the request compiles against,
    /// adopted or not; none when it compiles without a PCH.
    async::task<std::optional<std::string>> ensure_pch(const Ticket& ticket,
                                                       std::uint64_t epoch,
                                                       llvm::StringRef text,
                                                       std::optional<std::uint32_t> point,
                                                       const std::string& directory,
                                                       const std::vector<std::string>& arguments,
                                                       const Resolution& resolution,
                                                       const PCMBuilder::ModuleDeps& imports);

    /// The document's round moved on to `phase`: the client follows it.
    void enter_phase(Fid path_id, CompilePhase phase);

    /// The latest compile is no longer current: its epoch moves on.
    void mark_not_current(Fid path_id);

    /// Push the document's output to the client: `compiled` when a compile
    /// produced it, not when only the diagnostics it shows changed.
    void push_output(const Session& session, bool compiled);

    /// Replace some fields of the document's latest compile, keeping the
    /// rest (readers holding the old shared_ptr are unaffected).
    void set_pch_keys(Fid path_id,
                      std::optional<std::string> pch_key,
                      std::optional<std::string> base_pch_key);
    void set_output(Fid path_id, CompileOutput output);

    /// A background index run of the file reported `diagnostics` for the
    /// text hashing to `text_hash`: what the editor's compile, which skips
    /// the function bodies of the headers the file includes at its top, can
    /// get wrong (feature::skipped_diagnostics). They show on the document
    /// while it holds that text, open now or later
    /// (shown_skipped_diagnostics).
    void report_skipped_diagnostics(Fid path_id,
                                    std::uint64_t text_hash,
                                    std::vector<feature::FileDiagnostic> diagnostics);

    /// What of the file's last background run shows beside `output`, a
    /// compile of the document's text: what the compile does not report
    /// itself, and the warnings it holds back that the run raises too.
    /// Nothing when the run read other text, when the compile had no
    /// database entry of the file's, which the run compiles with, or when
    /// the user chose the file's context, which the run does not compile in.
    std::vector<feature::FileDiagnostic>
        shown_skipped_diagnostics(const Session& session, const CompileOutput& output) const;

    // answer.cpp

    /// Send an AST query of `kind` to the worker holding the document, which
    /// the caller compiled. A resend after a worker death recompiles first —
    /// the AST died with the worker — and a worker that no longer holds the
    /// document answers document_unloaded, which compiles it there and asks
    /// once more. `unanswered` reports a query never sent: its kind stopped
    /// by a crash, or a recompile that produced no AST.
    template <typename Params>
    RequestResult<Params> send_ast(const Ticket& ticket,
                                   feature::RequestKind kind,
                                   const Params& params,
                                   async::cancellation_token token,
                                   bool& unanswered);

    /// Send a stateless request of `kind` carrying the document's content,
    /// which `attempt` holds from its first admission on: admitted again
    /// once it has its slot and before every resend. `refused` reports one
    /// never sent. `parses`: see admits.
    template <typename Params>
    RequestResult<Params> send_stateless(const Ticket& ticket,
                                         feature::RequestKind kind,
                                         const Params& params,
                                         const Session::Attempt& attempt,
                                         bool parses,
                                         async::cancellation_token token,
                                         bool& refused);

    // command.cpp

    /// Take the tree `request` names, or wait for the run taking it. False
    /// when the run took none: the pool stopped, the run was cancelled, or
    /// the worker failed for a cause not the unit's.
    async::task<bool> take_include_tree(TreeRequest request);

    /// The run taking the tree, which belongs to no asker: one cancelled
    /// leaves it to the others waiting on `done`, set when it ends.
    async::task<> run_include_tree(TreeRequest request, std::shared_ptr<async::event> done);

    // changes.cpp

    /// The databases' reloads `delta` reports changed the build: the
    /// dependency graph follows, every file whose command came, changed or
    /// went recompiles and reindexes, and routing follows.
    void build_changed(CDBDiff delta);

    /// The end of every batch of changes: the context choices are checked
    /// again, the background index runs what the batch queued, and a client
    /// holding answers of open documents that went stale is asked to pull
    /// again (`refresh`).
    void finish_changes(bool refresh);

    /// Rescan the file's disk state (include edges, module maps). A module
    /// name the rescan gave its first provider cascades to the consumers
    /// holding sentinel edges against it; a name the file stopped
    /// providing cascades through the provider's real node instead.
    void rescan_disk_state(Fid path_id, bool& refresh);

    /// The cascade for "this file's on-disk content is new": rescan the
    /// file's disk state, then split every affected file into open
    /// (recompile) and closed (reindex).
    void cascade_disk_content_change(Fid path_id, bool& refresh);

    /// Cascade a module unit's compile-graph invalidation (its PCM and the
    /// units importing it), splitting dirtied units open/closed.
    void cascade_compile_graph(Fid path_id, bool& refresh);

    /// Treat every reader of a file that changed or vanished: open ones
    /// recompile, closed ones reindex, module units check their PCM again
    /// (which voids a build in flight).
    void mark_readers(llvm::ArrayRef<Fid> readers, bool& refresh);

    /// A module name just gained its first provider: cascade through its
    /// sentinel node and route the dirtied consumers to recompiles and
    /// ContentChanged reindexes.
    void provider_appeared(llvm::StringRef module_name, bool& refresh);

    /// The providers of `module_name` but `except` changed: the importers
    /// of every other one choose among them again.
    void cascade_providers(llvm::StringRef module_name, Fid except, bool& refresh);

    /// A dependency of the file changed: it recompiles when it is open, and
    /// reindexes unless its document's compile serves it.
    void mark_dependent(Fid path_id, bool& refresh);

    /// What a file's inputs changing does to its open document, if any
    /// (inputs_changed); `refresh` is set when the document is compiled,
    /// whose client is then asked to pull again.
    void mark_stale(Fid path_id, bool& refresh);

    /// Queue the file's background index again: `content` when its own
    /// content changed, else only a dependency's.
    void reindex(Fid path_id, bool content);

    /// What the disk holds for each of a database's inputs, in
    /// CompilationDatabase::inputs order: the content hash, nullopt when
    /// there are no bytes to read.
    using Hashes = llvm::SmallVector<std::optional<std::uint64_t>>;

    /// One registered database's watch state.
    struct WatchedDatabase {
        SourceID id;

        /// What was last adopted: the reads of the database's last load, or
        /// a deletion acted on.
        Hashes applied;

        /// Half-write guard: what the previous tick saw while it differed
        /// from `applied`; a tick seeing it again reloads.
        std::optional<Hashes> pending;

        /// The database's inputs, in CompilationDatabase::inputs order. The
        /// database is watched by the path the source names: a symlinked
        /// one may be pointed elsewhere since its load.
        llvm::SmallVector<std::shared_ptr<const vfs::Flag>, 0> inputs;
    };

    /// The hashes of the database's last load.
    Hashes loaded_hashes(SourceID id) const;

    /// Watch the response files the database's last load read, besides the
    /// database.
    void watch_inputs(WatchedDatabase& database);

    /// Register `id` for watching, from its last load.
    void watch_database(SourceID id);

    /// When no rule declares a source, register and watch the databases
    /// found at the watched places: the root and its direct subdirectories
    /// (listed again when the root directory moves), and every directory
    /// from an open document still without a command up to the root —
    /// which is how a database generated after startup is picked up.
    void discover_databases();

    /// Watch the HEAD and index of the git checkout the project lies in.
    void watch_checkout();

    /// One database's tick, its reload's delta merged into `delta`. Once
    /// its inputs have held the same other content for two consecutive
    /// ticks (or `force`), it reloads. A discovered database vanishing or
    /// returning flips its presence, and the files whose default entry
    /// moves with it change command (see Build::source_order); its entries
    /// keep serving meanwhile.
    void tick_database(WatchedDatabase& database, bool force, CDBDiff& delta);

    /// The files the database and another one both list: the ones whose
    /// default entry may move with the database's presence.
    llvm::SmallVector<Fid> shared_files(SourceID id) const;

    /// The source of each file's default entry, as the build ranks them
    /// now; none for a file the build no longer compiles.
    llvm::SmallVector<std::optional<SourceID>> default_sources(llvm::ArrayRef<Fid> files) const;

private:
    /// The open documents routed here.
    llvm::DenseMap<Fid, ServedDocument> documents;

    /// start() ran: documents adopted from now on have their serving
    /// decided at once.
    bool started = false;

    /// The build configuration the project activates (LoadedSettings).
    std::string configuration;

    /// start() found no compile command for the project. The guidance
    /// saying so waits for the first document the project serves: a folder
    /// of a multi-root workspace may hold no C or C++ at all.
    bool commands_missing = false;

    bool endpoint_recorded = false;

    bool metadata_flush_scheduled = false;

    /// The pump's progress, relayed to the master, and its index changes.
    Signal<>::Connection progress_conn;
    Signal<llvm::ArrayRef<Fid>>::Connection index_changed_conn;

    /// Detached ensure_compiled joins from request_compile; the rounds
    /// live in the graph's task group.
    async::task_group<> started_compiles;

    /// The cancel of each document's in-flight compile send, which edited()
    /// and stop_compiles() fire to interrupt the worker's parse.
    llvm::DenseMap<Fid, std::unique_ptr<async::cancellation_source>> compile_interrupts;

    /// A background run's diagnostics of a file (see
    /// report_skipped_diagnostics).
    struct SkippedDiagnostics {
        std::uint64_t text_hash = 0;

        std::vector<feature::FileDiagnostic> diagnostics;
    };

    /// The last background run's diagnostics of every file, open or not.
    llvm::DenseMap<Fid, SkippedDiagnostics> skipped;

    /// The include-tree runs in flight, by tree: a second asker waits for
    /// the first.
    llvm::DenseMap<UnitCommand, std::shared_ptr<async::event>> tree_runs;

    async::task_group<> tree_tasks;

    /// Whether start() began watching the databases: never for the rootless
    /// project, nor before it starts.
    bool watching = false;

    llvm::SmallVector<WatchedDatabase> watched_databases;

    /// The root directory, whose stamp moves when an entry appears or goes.
    std::shared_ptr<const vfs::Flag> root_flag;

    /// The root's stamp at the listing `listed` came from; nullopt while no
    /// listing can be trusted to hold.
    std::optional<vfs::Stamp> listed_at;

    /// The places database_places names under the root.
    llvm::SmallVector<Spelling> listed;

    /// The watched places with no database registered there yet.
    llvm::StringMap<std::shared_ptr<const vfs::Flag>> places;

    /// The places a registered database is known to stand at.
    llvm::StringSet<> registered;

    /// The checkout's HEAD and index: every git operation that changes the
    /// worktree rewrites the index, and then every file under the project
    /// root is due for a look.
    llvm::SmallVector<std::shared_ptr<const vfs::Flag>> checkout;

    /// The project's background tasks (checkpoints, flushes, polls, the
    /// control listener); cancelled and joined in shutdown().
    async::task_group<> bg_tasks;
};

template <typename Outcome>
Outcome ServedProject::finish(const Ticket& ticket,
                              feature::RequestKind kind,
                              llvm::StringRef label,
                              Outcome result,
                              bool snapshot) {
    auto& session = *ticket.session;
    if(!result.has_value()) {
        if(!worker::is_operational_error(result.error())) {
            LOG_ANOMALY(WorkerRequestFail,
                        "{} failed for {}: {}",
                        label,
                        project.file_table.resolve(session.path_id),
                        result.error().message);
            return result;
        }
        // A client's own cancel stays a cancel.
        if(result.error().code == worker::dispatch_errc::cancelled) {
            return result;
        }
    }
    // Answered or not, a stale request tells the client to re-pull.
    if(!ticket.fresh() && !snapshot) {
        return Outcome{kotatsu::outcome_error(content_modified())};
    }
    if(!result.has_value()) {
        LOG_INFO("{} for {} answers empty: {}",
                 label,
                 project.file_table.resolve(session.path_id),
                 result.error().message);
        return empty_value<typename Outcome::value_type>();
    }
    // The reply proves this kind on the DISPATCHED content answers; an edit
    // that arrived mid-flight must not clear the new content's record —
    // crashes were counted per attempt regardless of staleness, a success
    // clears only when fresh. Clearing a record republishes: no compile
    // runs to drop its note.
    if(session.succeeded(kind, ticket.generation)) {
        republish(ticket.session);
    }
    return result;
}

template <typename Params>
RequestResult<Params> ServedProject::send_ast(const Ticket& ticket,
                                              feature::RequestKind kind,
                                              const Params& params,
                                              async::cancellation_token token,
                                              bool& unanswered) {
    auto& session = *ticket.session;
    auto unsent = [&] {
        unanswered = true;
        return kotatsu::outcome_error(
            ipc::Error{worker::dispatch_errc::cancelled, "Query not sent"});
    };
    // The caller's compile gave a crash of the kind time to be booked.
    if(!admits(session, kind, nullptr, false)) {
        co_return unsent();
    }
    // A query that kills the worker is this document's doing even though
    // its compile finished: per-kind record, since only this query kind
    // answering clears it (see Session).
    Session::Attempt attempt(session, kind);
    bool compile = false;
    auto& pool = sched.pool;
    co_return co_await deliver(
        pool,
        true,
        [&]() -> RequestResult<Params> {
            while(true) {
                if(std::exchange(compile, true) && !co_await ensure_compiled(ticket.session)) {
                    co_return unsent();
                }
                if(!admits(session, kind, &attempt, false)) {
                    co_return unsent();
                }
                auto result =
                    co_await pool.send_stateful(session.path_id.raw, params, {.token = token});
                if(result.has_value() ||
                   result.error().code != worker::dispatch_errc::document_unloaded) {
                    co_return std::move(result);
                }
                // The worker evicted the document behind a compile that
                // still reads current: its eviction notice crossed a compile
                // of the document still finishing, which the master took for
                // the one that would put it back, or another document's
                // request arrived between this compile's reply and the query.
                // Compile it there again. Each eviction takes another request
                // arriving in that window, so the retries end with the work in
                // flight.
                invalidate(session.path_id);
            }
        },
        [&](const ipc::Error& error) {
            record_crash(ticket.session, kind, worker::death_of(error), error.message);
        });
}

template <typename Params>
async::task<typename protocol::RequestTraits<Params>::Result, ipc::Error>
    ServedProject::ast_request(const Ticket& ticket,
                               feature::RequestKind kind,
                               llvm::StringRef label,
                               Params params,
                               async::cancellation_token token) {
    using Result = typename protocol::RequestTraits<Params>::Result;
    auto& session = *ticket.session;
    params.path = std::string(project.file_table.resolve(session.path_id));

    if(!admits(session, kind, nullptr, false)) {
        co_return Result{};
    }

    ScopedTimer timer;
    if(!co_await ensure_compiled(ticket.session)) {
        if(!ticket.fresh()) {
            co_await async::fail(content_modified());
        }
        co_return Result{};
    }
    if(!ticket.fresh()) {
        co_await async::fail(content_modified());
    }
    auto wait_ms = timer.ms_f();

    bool unanswered = false;
    auto result = co_await send_ast(ticket, kind, params, token, unanswered);
    if(unanswered) {
        if(!ticket.fresh()) {
            co_await async::fail(content_modified());
        }
        co_return Result{};
    }
    result = finish(ticket, kind, label, std::move(result));
    if(result.has_value()) {
        LOG_PERF("request",
                 "kind={} file={} wait_ms={:.2f} total_ms={:.2f}",
                 label,
                 params.path,
                 wait_ms,
                 timer.ms_f());
    }
    co_return std::move(result);
}

template <typename Params>
RequestResult<Params> ServedProject::send_stateless(const Ticket& ticket,
                                                    feature::RequestKind kind,
                                                    const Params& params,
                                                    const Session::Attempt& attempt,
                                                    bool parses,
                                                    async::cancellation_token token,
                                                    bool& refused) {
    auto& session = *ticket.session;
    auto admitted = [&] {
        refused = !admits(session, kind, &attempt, parses);
        return !refused;
    };
    auto& pool = sched.pool;
    co_return co_await deliver(
        pool,
        false,
        [&] {
            return pool
                .send_stateless(params, worker::Priority::High, token, {}, nullptr, admitted);
        },
        [&](const ipc::Error& error) {
            record_crash(ticket.session, kind, worker::death_of(error), error.message);
        });
}

template <typename Params>
async::task<typename protocol::RequestTraits<Params>::Result, ipc::Error>
    ServedProject::stateless_request(const Ticket& ticket,
                                     feature::RequestKind kind,
                                     llvm::StringRef label,
                                     std::uint32_t offset,
                                     Params params,
                                     async::cancellation_token token) {
    using Result = typename protocol::RequestTraits<Params>::Result;
    auto& session = *ticket.session;
    auto path_id = session.path_id;
    auto path = std::string(project.file_table.resolve(path_id));
    // This build parses the same content as the compile: a compile or
    // preamble that crashed stops it as well as its own crashes do.
    if(!admits(session, kind, nullptr, true)) {
        LOG_DEBUG("{}: {} is stopped by a crash", label, path);
        co_return empty_value<Result>();
    }
    Session::Attempt attempt(session, kind);

    params.file = path;
    params.text = session.text().str();
    params.offset = offset;
    if(!loaded.is_set()) {
        co_await loaded.wait();
    }
    auto resolution = co_await document_command(path_id, params.directory, params.arguments);
    params.settings = project.settings;

    ScopedTimer timer;
    StatelessInputs inputs;
    if(!co_await prepare_stateless_inputs(ticket,
                                          params.text,
                                          params.offset,
                                          params.directory,
                                          params.arguments,
                                          resolution,
                                          inputs)) {
        co_await async::fail(
            ipc::Error{protocol::ErrorCode::InvalidRequest, "the server is shutting down"});
    }
    params.pch = std::move(inputs.pch);
    params.pcms = std::move(inputs.pcms);
    auto wait_ms = timer.ms_f();

    // A completion reply stays useful after edits at or past the cursor:
    // its ranges still hold, and the client filters it by what was typed
    // meanwhile.
    auto carried = params.text.substr(0, params.offset);
    auto snapshot = [&] {
        return std::is_same_v<Params, worker::CompletionParams> &&
               session.text().starts_with(carried);
    };
    if(!ticket.fresh() && !snapshot()) {
        co_await async::fail(content_modified());
    }

    if(resolution.synthesized) {
        params.synthesized = resolution.synthesized->files;
    }
    params.hidden = std::move(resolution.hidden);

    // A preamble crash inside the preparation, or a crash of a request like
    // this one while it was prepared or waited for a worker, stops the
    // content: it is not dispatched again.
    bool refused = false;
    auto result = co_await send_stateless(ticket, kind, params, attempt, true, token, refused);
    if(refused) {
        LOG_DEBUG("{}: {} was stopped by a crash before it reached a worker", label, path);
        if(!ticket.fresh() && !snapshot()) {
            co_await async::fail(content_modified());
        }
        co_return empty_value<Result>();
    }
    result = finish(ticket, kind, label, std::move(result), snapshot());
    if(result.has_value()) {
        LOG_PERF("request",
                 "kind={} file={} wait_ms={:.2f} total_ms={:.2f}",
                 label,
                 path,
                 wait_ms,
                 timer.ms_f());
    }
    co_return std::move(result);
}

}  // namespace clice

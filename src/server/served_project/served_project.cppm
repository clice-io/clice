module clice:server.served_project;

import :command.database;
import :command.search_config;
import :feature.diagnostics;
import :index.query;
import :index.shard;
import :index.tu_index;
import :project.background;
import :project.command;
import :project.graph.pch;
import :project.graph.pcm;
import :project.graph.task_graph;
import :project.project;
import :server.session;
import :settings.settings;
import :support.anomaly;
import :support.logging;
import :support.prelude;
import :support.signal;
import :support.timer;
import :syntax.position;
import :syntax.scan;
import :syntax.source_text;
import :syntax.token;
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

/// What a document's compile attempt waits on.
enum class CompilePhase : std::uint8_t {
    /// No attempt runs.
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

/// What the AST of a document's compile answered about the whole document,
/// and of a ranged request the last range asked: while the compile is
/// current, an evicted document answers from them (ServedProject::
/// ast_request).
struct KeptAnswers {
    protocol::RequestTraits<worker::SemanticTokensParams>::Result semantic_tokens;

    protocol::RequestTraits<worker::FoldingRangeParams>::Result folding_ranges;

    protocol::RequestTraits<worker::DocumentSymbolParams>::Result document_symbols;

    std::optional<protocol::RequestTraits<worker::DocumentLinkParams>::Result> document_links;

    std::optional<
        std::pair<LocalSourceRange, protocol::RequestTraits<worker::InlayHintParams>::Result>>
        inlay_hints;
};

/// The kept answer to `params`, when one covers it.
template <typename Params>
std::optional<typename protocol::RequestTraits<Params>::Result> kept_answer(const KeptAnswers& kept,
                                                                            const Params& params) {
    if constexpr(std::same_as<Params, worker::SemanticTokensParams>) {
        return kept.semantic_tokens ? std::optional(kept.semantic_tokens) : std::nullopt;
    } else if constexpr(std::same_as<Params, worker::FoldingRangeParams>) {
        return kept.folding_ranges ? std::optional(kept.folding_ranges) : std::nullopt;
    } else if constexpr(std::same_as<Params, worker::DocumentSymbolParams>) {
        return kept.document_symbols ? std::optional(kept.document_symbols) : std::nullopt;
    } else if constexpr(std::same_as<Params, worker::DocumentLinkParams>) {
        return kept.document_links;
    } else if constexpr(std::same_as<Params, worker::InlayHintParams>) {
        if(kept.inlay_hints && kept.inlay_hints->first == params.range) {
            return kept.inlay_hints->second;
        }
        return std::nullopt;
    } else {
        return std::nullopt;
    }
}

/// Keep `answer`, the AST's to `params`, if its kind is kept; an empty
/// optional says the worker had no AST, and answers nothing.
template <typename Params>
void keep_answer(KeptAnswers& kept,
                 const Params& params,
                 const typename protocol::RequestTraits<Params>::Result& answer) {
    if constexpr(std::same_as<Params, worker::SemanticTokensParams>) {
        if(answer) {
            kept.semantic_tokens = answer;
        }
    } else if constexpr(std::same_as<Params, worker::FoldingRangeParams>) {
        if(answer) {
            kept.folding_ranges = answer;
        }
    } else if constexpr(std::same_as<Params, worker::DocumentSymbolParams>) {
        if(answer) {
            kept.document_symbols = answer;
        }
    } else if constexpr(std::same_as<Params, worker::DocumentLinkParams>) {
        kept.document_links = answer;
    } else if constexpr(std::same_as<Params, worker::InlayHintParams>) {
        kept.inlay_hints.emplace(params.range, answer);
    }
}

/// One open document's compilation products, read by everything that
/// answers from the document's compile. Immutable once published but for
/// its kept answers: every change installs a fresh one (copy-on-write over
/// the cheap members), so a reader that copied the shared_ptr may keep
/// using it across suspension points.
struct DocumentCompile {
    /// Content key into Project::pchs for the PCH the document's parse
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

    /// Content key into Project::pchs for the precompiled header the
    /// build declares for the document's command, when the parse loads it:
    /// under its preamble's PCH, or as pch_key itself.
    std::optional<std::string> base_pch_key;

    /// The host whose command the compile borrowed; invalid for none.
    Fid host;

    /// The command the compile ran under (SelectedCommand::ref): a change of the
    /// members that moves the document's selection — its command or its
    /// host — recompiles it.
    CommandID command = invalid_command;

    /// The key of the PCH the parse loaded.
    const std::optional<std::string>& loaded_pch() const {
        return pch_failed ? base_pch_key : pch_key;
    }

    /// The pch.idx index of the preamble's PCH the parse loaded, while it
    /// is the preamble's (Project::preamble_index): held here, it stays
    /// loaded while a compile of a document reads it.
    std::shared_ptr<index::TUIndex> preamble_index;

    /// The generation of the document's graph node the compile ran at
    /// (TaskGraph::generation): the compile describes the buffer and what it
    /// read while that is still the node's. None before a compile finished,
    /// and once one produced no result.
    std::optional<std::uint64_t> generation;

    /// The latest compilation's TU index; null until a compile commits
    /// index data. NOT merged into Project.index — that only gets
    /// disk-derived data from background indexing.
    std::shared_ptr<index::TUIndex> index;

    /// The focus file's code the compile's preprocessor skipped (flat
    /// begin/end offset pairs): what the file's other contexts may compile.
    std::vector<std::uint32_t> inactive_regions;

    /// Publishable products of the latest compilation (see CompileOutput).
    std::optional<CompileOutput> output;

    /// Shared by the copies of the same compile: the requests answered from
    /// its AST add to them.
    std::shared_ptr<KeptAnswers> kept = std::make_shared<KeptAnswers>();
};

/// An open document as the project serving it sees it: its latest compile,
/// whether the document is compiled or served from the index, and whether
/// its command was borrowed or guessed.
struct ServedDocument {
    std::shared_ptr<Session> session;

    /// Null before the document's first compile.
    std::shared_ptr<const DocumentCompile> latest;

    /// Dependency snapshot from the last successful compile, used for
    /// staleness detection.
    std::optional<Inputs> inputs;

    /// See ServingMode for the write discipline.
    ServingMode serving = ServingMode::Escalated;

    /// The latest compile's file index merged with the file's shard's
    /// entries in the code the compile skipped (index::OpenBuffer::merged):
    /// made on first read while the buffer holds the very bytes both
    /// indexed, dropped when it no longer does, made again when the shard
    /// changes (ServedProject::merge_again).
    struct MergedIndex {
        /// The compile's TU index it merges, which the copies of one
        /// compile share.
        std::shared_ptr<index::TUIndex> index;

        std::shared_ptr<const index::Shard> shard;

        /// Zero when the shard adds nothing to the compile's index, else a
        /// hash of the merged index's entries: two tell whether the
        /// additions changed.
        std::uint64_t additions = 0;
    };

    mutable std::optional<MergedIndex> merged;
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
    AST,
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
    /// makes it re-pull. Await the didOpen index_first once (bounded by one index
    /// attempt) before answering Empty, and decide again from whatever
    /// source is there once it finishes: the shard, the escalated compile,
    /// or honestly empty under readonly "on".
    bool await_cold_attempt = false;
};

struct EnsureOptions {
    /// The caller asks the AST, not only the compile's output: a current
    /// document no worker holds compiles again.
    bool ast = true;
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
    if constexpr(std::is_same_v<Value, codec::raw_value>) {
        return codec::raw_value{"null"};
    } else {
        return Value{};
    }
}

/// One project the server serves: the project on disk, which builds over
/// the process's worker pool, the open documents the master
/// routed to it, and everything the project derives for them. Its work is
/// split in files: lifetime, adoption and the open documents' commands
/// (served_project.cpp), compiling the open documents (compile.cpp), what
/// answers a request on them now and asking the workers (answer.cpp), and
/// handling changes of its inputs (changes.cpp). Shared: a request in
/// flight keeps a removed project alive until it completes.
class ServedProject final : public std::enable_shared_from_this<ServedProject> {
public:
    /// `root` is the project's directory; empty for the rootless project a
    /// server without folders runs, which loads nothing. What the project
    /// tells of its index reaches the documents: an index attempt ending
    /// (the index_first in decide_serving promised the index would serve the
    /// cold document, and one that finishes without a servable shard ends
    /// that promise — it escalates like the disabled-indexing branch, or the
    /// document answers empty until its first edit; its run's skipped-body
    /// diagnostics show), shards changing (documents without a current file
    /// index serve these very shards, freshness clause 4: the client asks
    /// again for what it pulled from them), and the records beside the index
    /// changing (schedule_metadata_flush).
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

    /// Quiesce the compiles, then the project's own shutdown: the background
    /// index stops, the final state is persisted and the store closes.
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

    /// After the editor saved a file routed here: record it as a provisional
    /// member (Project::record_provisional).
    void record_saved(Fid path_id);

    // compile.cpp

    /// Compile an open document's AST if it is not current, or the caller
    /// asks for the AST and no worker holds it: join the document's attempt
    /// (spawning one if none is live) and retry through stale attempts until
    /// a terminal result. Returns true when the latest compile is current on
    /// return, and resident when the caller holds the document
    /// (DocumentHold). An edit of the document abandons the join — the
    /// editor re-requests after an edit, so the result would describe a
    /// buffer that no longer exists.
    async::task<bool> ensure_compiled(std::shared_ptr<Session> session, EnsureOptions options = {});

    /// Whether the document's compile is current, with what it read looked
    /// at: an evicted document's answers must show a header changed while
    /// it was out, which polling or indexing off would not report.
    bool confirm_current(Session& session);

    /// Start (or join) the document's compile detached — the caller keeps
    /// serving from the index until it finishes. The graph's
    /// one-attempt-per-node discipline dedupes concurrent requests.
    void request_compile(std::shared_ptr<Session> session);

    /// The escalation triggers' single write point: flip an index-only
    /// document to Escalated. The build stays pull-driven — the next
    /// request that needs the AST starts it. A no-op for already-escalated
    /// documents and under readonly = "on" (the mode transition is
    /// disabled).
    void escalate(Fid path_id);

    /// The document was edited: the latest compile is no longer current and
    /// the in-flight attempt's result is void. Fires the attempt's advisory
    /// token and cancels its compile over IPC, which interrupts the
    /// worker's parse (the attempt still observes the real reply: crash
    /// accounting depends on it); the document is compiled from now on
    /// (escalate).
    void edited(Session& session);

    /// The document compiles in another context now: a different
    /// compilation identity. Make any in-flight compile outdated and drop the
    /// state earned under the old one — including the self-containment
    /// check, since a different host can change the macro environment.
    void choice_changed(Session& session);

    /// An invalidation that leaves the buffer (a dependency changed on disk,
    /// a save retrying a crash): the latest compile is no longer current and
    /// an in-flight attempt must not commit as such, but its parse keeps
    /// running — the attempt publishes the product as bounded staleness
    /// before reporting Stale.
    void invalidate(Fid path_id);

    /// didSave: every crashed kind of the document retries on its next
    /// request, the PCH and PCMs it consumes included, and so do the ones
    /// whose build failed; a header found self-contained whose compile
    /// misses names, or has not finished, re-runs its self-containment
    /// check.
    void retry_after_save(Session& session);

    /// What the document's compile read, one snapshot at a time: its own
    /// inputs, its header context's chain, its PCH's and those of the modules
    /// it imports — of a module that failed to build, what the failure
    /// rests on (Project::module_inputs). Both the staleness check and the
    /// change handling's readers go through it: what a request finds
    /// changed and what a disk change reaches are the same files.
    void each_input(Fid path_id, llvm::function_ref<void(const Inputs&)> visit) const;

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

    /// Whether a stateful worker holds the document's AST.
    bool resident(Fid path_id) const {
        return project.pool->resident(path_id.raw);
    }

    /// The document's latest compile, or null before its first compile.
    std::shared_ptr<const DocumentCompile> latest(Fid path_id) const;

    /// The document's latest compile while its output describes buffer
    /// `version`, or null.
    std::shared_ptr<const DocumentCompile> latest_at(Fid path_id, int version) const;

    /// The latest compile describes the buffer and what it read: no
    /// invalidation reached the document's node since it ran. Whether a
    /// worker still holds its AST is the other fact (resident).
    bool current(Fid path_id) const;

    /// Whether the latest compile's index describes the current buffer.
    /// The arbitration key of the index freshness contract (IndexQuery
    /// clauses 3-4): a current-but-indexless compile (no AST, or an index
    /// the worker could not send) is an honest gap, not a servable
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

    /// Interrupt every in-flight parse and wait for the detached compile
    /// joins. The attempts themselves finish in TaskGraph::shutdown — the
    /// interruption is what keeps that finish prompt (an attempt's stateful
    /// send carries the interrupt, not its advisory token: contract 2 wants
    /// the real reply, and the interrupt makes it arrive early).
    async::task<> stop_compiles();

    // served_project.cpp: the open documents' commands

    /// What the open document's command is resolved for: its active
    /// context choice, or `candidate` instead, in the editor.
    CommandRequest command_request(Fid path_id, const ContextChoice* candidate = nullptr) const;

    /// The file's context choice while it is this project's and holds.
    const ContextChoice* active_choice(Fid path_id) const;

    /// The host source of a header: its active choice's, else
    /// clice::host_of.
    Fid host_of(Fid path_id);

    /// The dialect the server lexes an open file's text in, for the results
    /// computed from its index, hover's directive lookup and completion's
    /// context checks: Project::dialect under the file's active choice.
    const clang::LangOptions& dialect(Fid path_id);

    // changes.cpp

    /// Files changed on disk (MasterServer::apply_disk_changes): the project
    /// follows those it knows (Project::knows, files_changed), and the open
    /// documents reading them recompile.
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

    /// Before a file's first compile: register, load and watch the
    /// databases in the directories from the file's up to the root, which
    /// startup discovery (the root and its direct subdirectories) did not
    /// look at — a file of a deeper project compiles from its own database —
    /// and follow the loads (Project::discover_databases).
    void discover_around(Fid path_id);

    /// The directories below the root whose configuration file decides what
    /// the project loads, the open documents' among them
    /// (Project::configuration_dirs).
    std::vector<CanonicalPath> configuration_dirs() const;

    /// The open documents and the files their compiles read, as the
    /// project's change handling needs them.
    llvm::SmallVector<OpenFile> open_files() const;

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
    /// or a close makes every ticket of the document outdated.
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
    /// failed, or the request is outdated.
    async::task<std::optional<GateStop>> navigation_gate(const Ticket& ticket);

    /// Await the document's compile; a GateStop when it failed or the
    /// request is outdated.
    async::task<std::optional<GateStop>> await_compile(const Ticket& ticket,
                                                       EnsureOptions options = {});

    /// The symbol under a position of the file's serving source, if any.
    std::optional<index::IndexQuery::Cursor> cursor_at(Fid path_id,
                                                       const protocol::Position& position) const;

    /// The open documents, as the reads of the index take them.
    std::vector<index::OpenBuffer> open_buffers() const;

    /// The code the open document's latest compile skipped while its
    /// buffer is the text the file's shard indexed: the code of the file's
    /// other contexts, which the answers the AST gives are completed with.
    struct SkippedCode {
        /// Flat begin/end offset pairs; empty while the buffer is not the
        /// shard's text.
        llvm::ArrayRef<std::uint32_t> regions;

        /// The index serving the document holds entries there.
        bool indexed = false;
    };

    SkippedCode skipped_code(Fid file) const;

    /// Merges the open document's compile with its file's shard again, the
    /// shard having changed: whether what the merge adds changed with it.
    bool merge_again(Fid file) const;

    /// The project's index over its open documents, for one synchronous
    /// read.
    index::IndexQuery query() const {
        return project.index_query.reading(open_buffers());
    }

    /// query(), also asking the other projects the master serves when it is
    /// taken, each over its own open documents (index::IndexQuery::asking):
    /// what navigation, references, the hierarchies, hover, workspace
    /// symbols and an inlay hint's targets read.
    index::IndexQuery query_all() const;

    /// What a rename or a code action reads: query() over the open documents
    /// of the projects nested in the root too, which are its files as well.
    /// Another project's open files read as closed ones, from the shards
    /// this project holds, and no other project is asked: the edits of a
    /// rename stay in the files it may edit.
    index::IndexQuery editing_query() const;

    /// The document's text, read in the dialect the server lexes it in.
    SourceText& text_of(Session& session);

    /// The header search of the command the open file compiles under.
    SearchConfig search_of(Fid path_id);

    /// The module declaration of `text`, the open file's own up to a point,
    /// as the preprocessor reads it under the file's command.
    ScanResult declared_module(Fid path_id, llvm::StringRef text);

    /// The preamble include links of a document's active PCH; empty when
    /// there is no PCH or its preamble no longer matches the buffer.
    std::vector<index::DocumentLink> preamble_links(const Session& session) const;

    /// The include-like directive links of a document's whole buffer: the
    /// PCH's cached preamble links, then the worker's AST's for the rest,
    /// and the index's in the code the compile skipped. Module names go
    /// through the ordinary index pipeline, not these.
    async::task<std::vector<index::DocumentLink>, ipc::Error>
        directive_links(const Ticket& ticket, async::cancellation_token token);

    /// Whether a dispatch of `kind` may reach a worker: its record does not
    /// stop it — or, once a Dispatch holds it, `kind` did not crash during
    /// its flight — and for a build that parses the document (`parses`),
    /// neither does the compile's or its preamble's. One check admits a
    /// dispatch, of every kind: before its inputs are prepared, and again
    /// where it reaches a worker — after a stateless request's wait in the
    /// queue and before every resend — since a request like it can crash on
    /// the same content meanwhile.
    bool admits(const Session& session,
                feature::RequestKind kind,
                const Session::Dispatch* dispatch,
                bool parses) const;

    /// Ask the worker holding the document's AST: compile it if it is not
    /// current, check its crash records, send, and finish; a missing AST
    /// answers an empty result. An evicted document answers from its
    /// compile's kept answers when they cover the request, and compiles
    /// again otherwise. `params` comes with the fields particular to
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
    /// compiles a buffer state the shared AST attempt may not have seen yet,
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
    template <typename R>
    R finish(const Ticket& ticket,
             feature::RequestKind kind,
             llvm::StringRef label,
             R result,
             bool snapshot = false);

    /// Set while compiles may run and requests resolve commands. A project
    /// made while its root's previous one still shuts down
    /// (MasterServer::serve_folders) holds documents before it loads, which
    /// would compile under no build at all; stop_compiles() lets them go
    /// too.
    async::event loaded{true};

    MasterServer& server;

    /// Built over the server's pool, its runners set at construction —
    /// nodes materialize on demand, so a module-free project pays nothing.
    /// The background index is serving-neutral; the document-side policy is
    /// installed into its hooks at construction.
    Project project;

    /// Problems found while reading the settings, kept so the client can
    /// publish them as diagnostics on the configuration file's URI, and the
    /// path of the file found (empty when none).
    std::vector<SettingsIssue> settings_issues;
    std::string settings_file;

private:
    friend struct testing::ServedProjectFixture;

    // served_project.cpp

    /// Decide a freshly opened index-only document's serving: escalate one
    /// that already diverged from its shard, or index the file first in the
    /// background when nothing can serve it. Escalated documents need no
    /// decision — builds are pull-driven.
    void decide_serving(ServedDocument& document);

    /// When this project holds its cache directory's writer lock, the
    /// commands that find it taken ask this server to index for them:
    /// listen on a loopback port and record it next to the lock (see
    /// index/writer_lock.cppm). The record is removed at shutdown.
    void start_control_listener();

    /// Schedule a save carrying dirty precompiled records or context choices; no-op
    /// when one is already scheduled or the server is shutting down (the
    /// final shutdown save covers it).
    void schedule_metadata_flush();

    /// Book a background run for the document's skipped-body diagnostics when
    /// it shows them and lacks those of its text, as after a restart, when
    /// the index is current and runs nothing, or after a change of its
    /// inputs dropped them: first in the queue, or (`first` off) in its
    /// turn. Only a file of its own database entry is reported on
    /// (UnitRequest::skipped_diagnostics).
    void book_skipped_diagnostics(const ServedDocument& document, bool first);
    async::task<> metadata_flush_task();

    /// The periodic Project::checkpoint().
    async::task<> checkpoint_task();

    /// The default-sources loop, poll_sources on the workspace poll's pace.
    /// Spawned by start() unless polling is off.
    async::task<> sources_poll_task();

    // compile.cpp

    /// A file the open document reads changed: its compile, its
    /// self-containment check and its stopped request kinds start over.
    void inputs_changed(Session& session);

    /// How to compile the open document: its AST node's work.
    Work compile_work(Fid path_id);

    struct CompileAttempt;

    /// One attempt of the document's compile (CompileAttempt).
    async::task<Result> compile(Attempt& attempt, Fid path_id);

    /// Whose fault a worker death under the document's compile is, tried
    /// in turn: the clang-tidy pass, when `tidy` ran it, the PCH pair
    /// `consumed` the parse read, else the document's own code. Returns
    /// whether the compile retries without the suspect: the pass left out,
    /// the pair retracted and rebuilt.
    bool attribute_crash(const std::shared_ptr<Session>& session,
                         bool tidy,
                         const std::optional<std::string>& consumed,
                         const ipc::Error& error);

    /// What a request's own parse (completion, signature help) learned of
    /// the document's PCH, built from the request's buffer (`built`): a
    /// preamble refused or crashed goes into the document's records, and the
    /// key the parse loads becomes the document's while the request's ticket
    /// and `generation`, the document's node's as the build began, hold — an
    /// edit or a choice change moves the ticket, an invalidation the
    /// generation, and either way the resolved command may describe a
    /// command that no longer exists: adopting the key would hand the
    /// document's preamble features and staleness checks a PCH of another
    /// command. What a partial preamble loads is the request's own.
    void adopt_pch(const Ticket& ticket, std::uint64_t generation, const BuiltPCH& built);

    /// The document's attempt moved on to `phase`: the client follows it.
    void enter_phase(Fid path_id, CompilePhase phase);

    /// Push the document's output to the client: `compiled` when a compile
    /// produced it, not when only the diagnostics it shows changed.
    void push_output(const Session& session, bool compiled);

    /// Replace the document's latest compile with a copy `edit` changes
    /// (readers holding the old shared_ptr are unaffected).
    void update_latest(Fid path_id, llvm::function_ref<void(DocumentCompile&)> edit);

    /// The file's last background run brought new skipped-body diagnostics:
    /// what the editor's compile, which skips the function bodies of the
    /// headers the file includes at its top, can get wrong
    /// (feature::skipped_diagnostics). They show on the document while it
    /// holds the text they describe — a compile owed shows them as it commits.
    void show_skipped_diagnostics(Fid path_id);

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
    /// the caller compiled. A resend after a worker death recompiles first:
    /// the AST died with the worker. `unanswered` reports a query never sent:
    /// its kind stopped by a crash, or a recompile that produced no AST.
    template <typename Params>
    RequestResult<Params> send_ast(const Ticket& ticket,
                                   feature::RequestKind kind,
                                   const Params& params,
                                   async::cancellation_token token,
                                   bool& unanswered);

    /// Send a stateless request of `kind` carrying the document's content,
    /// which `dispatch` holds from its first admission on: admitted again
    /// once it has its slot and before every resend. `refused` reports one
    /// never sent. `parses`: see admits.
    template <typename Params>
    RequestResult<Params> send_stateless(const Ticket& ticket,
                                         feature::RequestKind kind,
                                         const Params& params,
                                         const Session::Dispatch& dispatch,
                                         bool parses,
                                         async::cancellation_token token,
                                         bool& refused);

    // changes.cpp

    /// The build's commands changed (`delta`, what databases' reloads report,
    /// or none for a member just recorded): the project follows
    /// (Project::update_members), every open document whose command moved
    /// recompiles, and routing follows.
    void commands_changed(CDBDiff delta);

    /// What a change of the project's inputs did to the open documents: the
    /// stale ones compile again, the reindexed ones book their skipped-body
    /// diagnostics, and when commands changed, so do the ones whose command
    /// moved. Then the context choices are checked again, the background
    /// index runs what the change queued, a client holding answers of
    /// documents that went stale is asked to pull again, and when commands
    /// changed, routing and the watched settings follow.
    void follow(const Affected& affected);

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

    /// The background index's progress, relayed to the master.
    Signal<>::Connection progress_conn;

    Signal<Fid>::Connection attempt_conn;

    Signal<llvm::ArrayRef<Fid>>::Connection index_conn;

    Signal<>::Connection metadata_conn;

    /// Detached ensure_compiled joins from request_compile; the attempts
    /// live in the graph's task group.
    async::task_group<> started_compiles;

    /// The cancel of each document's in-flight compile send, which edited()
    /// and stop_compiles() fire to interrupt the worker's parse.
    llvm::DenseMap<Fid, std::unique_ptr<async::cancellation_source>> compile_interrupts;

    /// Whether start() began watching the databases: never for the rootless
    /// project, nor before it starts.
    bool watching = false;

    /// The project's background tasks (checkpoints, flushes, polls, the
    /// control listener); cancelled and joined in shutdown().
    async::task_group<> bg_tasks;
};

template <typename R>
R ServedProject::finish(const Ticket& ticket,
                        feature::RequestKind kind,
                        llvm::StringRef label,
                        R result,
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
        return R{kotatsu::outcome_error(content_modified())};
    }
    if(!result.has_value()) {
        LOG_INFO("{} for {} answers empty: {}",
                 label,
                 project.file_table.resolve(session.path_id),
                 result.error().message);
        return empty_value<typename R::value_type>();
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
    Session::Dispatch dispatch(session, kind);
    bool compile = false;
    auto& pool = *project.pool;
    co_return co_await deliver(
        pool,
        true,
        [&]() -> RequestResult<Params> {
            if(std::exchange(compile, true) && !co_await ensure_compiled(ticket.session)) {
                co_return unsent();
            }
            if(!admits(session, kind, &dispatch, false)) {
                co_return unsent();
            }
            co_return co_await pool.send_stateful(session.path_id.raw, params, {.token = token});
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

    // Nothing an evicted document's compile read changed: what its AST
    // answered still holds.
    if(!resident(session.path_id) && confirm_current(session)) {
        if(auto kept = kept_answer(*latest(session.path_id)->kept, params)) {
            co_return std::move(*kept);
        }
    }

    // From before the compile until the query answers: another document's
    // compile finishing meanwhile evicts some other AST.
    DocumentHold hold(*project.pool, session.path_id.raw);
    ScopedTimer timer;
    if(auto stop = co_await await_compile(ticket)) {
        if(stop->error) {
            co_await async::fail(std::move(*stop->error));
        }
        co_return Result{};
    }
    auto wait_ms = timer.ms_f();
    auto compile = latest(session.path_id);

    bool unanswered = false;
    auto result = co_await send_ast(ticket, kind, params, token, unanswered);
    if(unanswered) {
        if(!ticket.fresh()) {
            co_await async::fail(content_modified());
        }
        co_return Result{};
    }
    // The worker's own answer: finish turns a worker that could not answer
    // into an empty one, which is no answer to keep.
    if(result.has_value() && ticket.fresh()) {
        keep_answer(*compile->kept, params, *result);
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
                                                    const Session::Dispatch& dispatch,
                                                    bool parses,
                                                    async::cancellation_token token,
                                                    bool& refused) {
    auto& session = *ticket.session;
    auto admitted = [&] {
        refused = !admits(session, kind, &dispatch, parses);
        return !refused;
    };
    auto& pool = *project.pool;
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
    Session::Dispatch dispatch(session, kind);

    params.file = path;
    params.text = session.text().str();
    params.offset = offset;
    // Taken before the command resolves: after an invalidation meanwhile
    // the command may be stale, and adopt_pch refuses the PCH built under
    // it.
    auto generation = project.graph.generation(ast_node(path_id));
    if(!loaded.is_set()) {
        co_await loaded.wait();
    }
    auto resolution =
        co_await project.resolve_with_trees(path_id, [&] { return command_request(path_id); });
    params.directory = resolution.directory;
    params.arguments = resolution.arguments;
    params.settings = project.settings;

    // The scan and the PCH run under the request's command with the text it
    // compiles, so an unsaved `import m;` builds its PCM before the parse
    // needs it. An import that did not build is the parse's to report; none
    // only when the graph shut down.
    ScopedTimer timer;
    auto imports = co_await project.build_imports(path_id, params.text, resolution, {});
    auto built = imports ? co_await project.build_pch(path_id,
                                                      params.text,
                                                      params.offset,
                                                      resolution,
                                                      imports->imports,
                                                      {})
                         : std::nullopt;
    if(!built) {
        co_await async::fail(
            ipc::Error{protocol::ErrorCode::InvalidRequest, "the server is shutting down"});
    }
    adopt_pch(ticket, generation, *built);
    params.pch = std::move(built->pch);
    params.pcms = project.imported_pcms(imports->imports.resolved());
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
    auto result = co_await send_stateless(ticket, kind, params, dispatch, true, token, refused);
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

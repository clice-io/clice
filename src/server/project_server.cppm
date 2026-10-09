module;

#include "modules/prelude.h"

module clice:server.project_server;

import :config.config;
import :project.command_resolver;
import :project.index_store;
import :project.project;
import :sched.stack;
import :server.ast_family;
import :server.context_service;
import :server.dispatcher;
import :server.editor_context;
import :server.features;
import :server.invalidator;
import :server.live_sources;
import :server.session;
import :server.session_store;
import :support.signal;

namespace clice {

class FileTracker;
class MasterServer;

/// Everything the server runs for one project: the project on disk, its
/// command resolution and editor context, its scheduling stack over the
/// process's worker pool, the open documents routed to it with the AST
/// family compiling them, and the read services answering over both. The
/// master holds one per project and routes each file to one of them
/// (MasterServer::owner_of); nothing here knows about the others. Shared:
/// a request in flight keeps a removed project alive until it completes.
class ProjectServer : public std::enable_shared_from_this<ProjectServer> {
public:
    /// `root` is the project's directory; empty for the rootless project a
    /// server without folders runs, which loads nothing.
    ProjectServer(MasterServer& server, CanonicalPath root);
    ~ProjectServer();

    /// Load the configuration — clice.toml under the root, overlaid with
    /// the client's initializationOptions (`init_options`), then
    /// finalized — and apply its serving mode. A cache directory belongs
    /// to one project: when the one configured is among
    /// `taken_cache_dirs`, this project falls back to its clice.toml's,
    /// then the default, then runs without one.
    void configure(const std::optional<kota::codec::dyn::Value>& init_options,
                   llvm::ArrayRef<CanonicalPath> taken_cache_dirs);

    /// Load the project from disk (see bootstrap_project), restore the
    /// editor's context choices, and start its store-lifetime services;
    /// then watch its databases and files. Documents opened before are
    /// validated and settled against the loaded state.
    void start();

    /// Quiesce compile and index work and persist the final state; then
    /// close() releases the store — once the pool stopped at exit, or
    /// right away for a project removed while the pool serves others (a
    /// worker still finishing a cancelled build of it only fails its
    /// write).
    kota::task<> shutdown();
    void close();

    /// Open a document routed here with its buffer — an editor's didOpen,
    /// or one another project closed as it moved here. Its saved context choice is
    /// validated and its serving settled now, or by start() when the
    /// project has not started yet.
    void open_session(Fid path_id, std::string text, int version);

    /// Close the session — the editor's didClose, or the document moving
    /// to another project, which opens it with its buffer next. The
    /// diagnostics clear travels through the session's output + on_output
    /// signal; a transport whose client has not completed the handshake
    /// drops it (nothing was ever pushed, so there is nothing to clear).
    void close_session(Fid path_id);

    /// Before a file's first compile: register the databases discovery
    /// finds between its directory and the root (see
    /// FileTracker::discover_around) so the compile finds its entry.
    void discover_around(Fid path_id);

    /// After the editor saved a file routed here: record a source of the
    /// workspace the build does not declare as a provisional member
    /// (Build::record), the build following it as a database reload would.
    void saved(Fid path_id);

    /// After a tick looked at the flags: weigh what the looks found of the
    /// project's databases (see FileTracker::tick_cdb) and dispatch the
    /// reloads. Nothing while the project's polling is off.
    void tick_databases();

    /// The single entry point for file events: fold the batch through the
    /// Invalidator, then execute the resulting effects against the mutable
    /// services (sessions, editor context, background indexer).
    void dispatch(llvm::ArrayRef<FileEvent> events);

    /// The files every open document's compile depends on (see
    /// ASTFamily::closure).
    void open_closures(llvm::SmallVectorImpl<Fid>& files);

    /// Whether anything here derives from the file: an open document, a
    /// command, an include edge, index rows, a compile that read it or
    /// looked for it. The disk changes of files it does not know are not
    /// this project's to cascade.
    bool knows(Fid path_id);

    MasterServer& server;
    kota::event_loop& loop;
    CanonicalPath root;

    /// The open documents routed to this project, and their buffer-sync
    /// logic.
    SessionStore sessions;

    Project project;
    CommandResolver commands{project};

    /// The scheduling core the batch driver runs too, its families
    /// registered at construction — nodes materialize on demand, so a
    /// module-free project pays nothing. The store and the pump are
    /// serving-neutral; the session-side policy — unservable escalation,
    /// serving-row refresh — lives on this class and is installed into the
    /// pump's hooks at construction. The AST family is assembled here in
    /// the project's server: its rounds capture sessions, quarantine and
    /// publishing.
    SchedulingStack sched;
    EditorContext contexts{project, commands, sched.store.contexts};
    ASTFamily ast;

    Dispatcher dispatcher;
    ContextService context_service{project, contexts, ast};

    ServerLiveSources live_sources;
    index::FreshnessGate freshness{project.file_table};
    index::IndexQuery index_query;

    Features features;
    Invalidator invalidator;

    /// Polling of the project's databases, default sources and checkout.
    /// Created by start() once the project is loaded (null before that and
    /// for the rootless project); the master's ticks and its sources loop
    /// in bg_tasks drive it, and the clice/internal/poll test hook drives
    /// ticks directly.
    std::unique_ptr<FileTracker> tracker;

    /// Problems found while loading clice.toml, kept so LSPClient can
    /// publish them as diagnostics on the config file's URI, and the path
    /// of the file found (empty when none).
    std::vector<ConfigIssue> config_issues;
    std::string config_path;

    /// A metadata save is scheduled and has not started yet.
    bool metadata_flush_pending() const {
        return metadata_flush_scheduled;
    }

private:
    /// A fresh session for the document, replacing a live one.
    std::shared_ptr<Session> create_session(Fid path_id);

    /// Settle a freshly opened index-only buffer: escalate one that
    /// already diverged from its shard, or boost the file's background
    /// indexing when nothing can serve it. Escalated sessions need no
    /// settlement — builds are pull-driven.
    void settle_open_serving(std::shared_ptr<Session> session);

    /// start() ran: documents opened from now on are settled at once.
    bool started = false;

    /// An index attempt settled with no retry pending; a session waiting
    /// on the index with nothing servable will never be served by it —
    /// escalate instead of letting it answer empty forever.
    void index_attempt_settled(Fid path_id);

    /// Whether an open session serves this file's project rows (freshness
    /// clause 4) and a client already pulled some of them — the emit
    /// condition of MasterServer::on_serving_rows_changed.
    bool serves_session_rows(Fid path_id) const;

    /// Filter a store row-change report down to the sessions actually
    /// serving those rows and wake the transports.
    void index_rows_changed(llvm::ArrayRef<Fid> path_ids);

    Signal<llvm::ArrayRef<Fid>>::Connection index_rows_conn;

    /// Forward the AST family's outputs and the pump's progress to the
    /// master's signals, which the transports subscribe to.
    Signal<std::shared_ptr<Session>>::Connection output_conn;
    Signal<>::Connection progress_conn;

    /// When this project holds its cache directory's writer lock, the
    /// commands that find it taken ask this server to index for them:
    /// listen on a loopback port and record it next to the lock (see
    /// index/writer_lock.h). The record is removed at shutdown.
    void start_control_listener();
    bool endpoint_recorded = false;

    /// Schedule a save carrying dirty artifact/context metadata; no-op
    /// when one is already scheduled or the server is shutting down (the
    /// final shutdown save covers it).
    void schedule_metadata_flush();
    kota::task<> metadata_flush_task();
    bool metadata_flush_scheduled = false;

    /// The periodic SchedulingStack::checkpoint().
    kota::task<> checkpoint_task();

    /// Drop pch_cache metadata for blobs the store's LRU evicted from
    /// disk (see checkpoint_task).
    void drain_store_evictions();

    /// The file tracker's default-sources loop: each tick hands the
    /// tracker's event batch to dispatch(). Spawned by start() unless
    /// polling is off.
    kota::task<> sources_poll_task();

    /// The project's background tasks (checkpoints, flushes, polls, the
    /// control listener); cancelled and joined in shutdown().
    kota::task_group<> bg_tasks;
};

}  // namespace clice

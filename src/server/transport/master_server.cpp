#include "server/transport/master_server.h"

#include <list>
#include <memory>
#include <set>
#include <string>
#include <tuple>
#include <vector>

#include "version.h"
#include "index/writer_lock.h"
#include "sched/bootstrap.h"
#include "server/state/file_tracker.h"
#include "server/transport/control_server.h"
#include "server/transport/lsp_client.h"
#include "support/anomaly.h"
#include "support/cache_store.h"
#include "support/filesystem.h"
#include "support/logging.h"
#include "support/timer.h"
#include "worker/protocol.h"

#include "kota/async/async.h"
#include "kota/codec/json/json.h"
#include "kota/ipc/codec/json.h"
#include "kota/ipc/recording_transport.h"
#include "kota/ipc/transport.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Process.h"
#include "llvm/Support/xxhash.h"

namespace clice {

/// Retention bound of the notify log. Subscribers drain promptly, so only
/// messages that fire before any client attaches accumulate (a handful of
/// startup guidance reports in practice); the cap is a safety net, not a
/// working-set size.
constexpr static std::size_t notify_log_limit = 128;

MasterServer::MasterServer(kota::event_loop& loop,
                           std::string self_path,
                           std::string requested_configuration) :
    loop(loop), pool(loop), requested_configuration(std::move(requested_configuration)),
    bg_tasks(loop), self_path(std::move(self_path)) {
    // Documents opened before initialize land in this project: sessions
    // are plain state, and initialize re-routes them once folders exist.
    projects.push_back(std::make_unique<ProjectServer>(*this, std::string()));
    // The notify hook is process-wide because the logging layer cannot
    // depend on the server; the composition root owns it for the server's
    // lifetime and turns reports into state (notify_log) plus a wake-up
    // signal. Master-side reports only ever fire on the event-loop thread
    // (see support/anomaly.h), so no synchronization is needed here.
    logging::set_notify_hook([this](logging::NotifyLevel level, std::string_view message) {
        notify_log.push_back(NotifyMessage{level, std::string(message)});
        if(notify_log.size() > notify_log_limit) {
            notify_log.pop_front();
        }
        notify_seq += 1;
        on_notify.emit();
    });
}

MasterServer::~MasterServer() {
    logging::set_notify_hook(nullptr);
}

/// Dump every configuration layer of a project — the config file verbatim,
/// the client's initializationOptions overlay, and the merged result after
/// defaults. Absence is stated explicitly so "was my config even read?"
/// never needs a support round-trip; the stderr mirror puts all of it in
/// the editor's output panel.
static void log_configuration(const ProjectServer& project, llvm::StringRef init_options) {
    if(project.config_path.empty()) {
        LOG_INFO("Configuration file: Missing (project {})", project.root);
    } else {
        auto raw = fs::read(project.config_path);
        LOG_INFO("Configuration file {}:\n{}",
                 project.config_path,
                 raw ? *raw : std::string("<unreadable>"));
    }
    if(init_options.empty()) {
        LOG_INFO("initializationOptions: Missing");
    } else {
        auto pretty = kota::codec::json::prettify(init_options);
        LOG_INFO("initializationOptions:\n{}", pretty ? *pretty : init_options.str());
    }
    if(auto json = kota::codec::json::to_string(project.project.config)) {
        auto pretty = kota::codec::json::prettify(*json);
        LOG_INFO("Effective configuration:\n{}", pretty ? *pretty : *json);
    }
}

void MasterServer::initialize() {
    if(!workspace_roots.empty()) {
        projects.front()->root = workspace_roots.front();
        for(auto& root: llvm::drop_begin(workspace_roots)) {
            projects.push_back(std::make_unique<ProjectServer>(*this, root));
        }
    }
    std::vector<std::string> cache_dirs;
    for(auto& project: projects) {
        project->configure(init_options_json, cache_dirs);
        cache_dirs.push_back(project->project.config.project.cache_dir);
    }

    // One pool serves every project, sized for the most demanding one;
    // the first project names the log directory, as the root did before
    // there were several.
    auto& first = projects.front()->project.config.project;
    WorkerPoolOptions pool_opts;
    pool_opts.self_path = self_path;
    pool_opts.stateful_count = first.stateful_worker_count;
    pool_opts.stateless_count = first.stateless_worker_count;
    pool_opts.min_stateless = first.min_stateless_worker_count;
    pool_opts.max_stateless = first.max_stateless_worker_count;
    for(auto& project: llvm::drop_begin(projects)) {
        auto& cfg = project->project.config.project;
        pool_opts.stateful_count =
            std::max<std::uint32_t>(pool_opts.stateful_count, cfg.stateful_worker_count.value);
        pool_opts.stateless_count =
            std::max<std::uint32_t>(pool_opts.stateless_count, cfg.stateless_worker_count.value);
        pool_opts.min_stateless =
            std::max<std::uint32_t>(pool_opts.min_stateless, cfg.min_stateless_worker_count.value);
        // 0 leaves the ceiling to the core count, the largest one.
        std::uint32_t ceiling = cfg.max_stateless_worker_count.value;
        pool_opts.max_stateless = pool_opts.max_stateless == 0 || ceiling == 0
                                      ? 0
                                      : std::max(pool_opts.max_stateless, ceiling);
    }

    if(!first.logging_dir.empty()) {
        session_log_dir = logging::session_log_directory(first.logging_dir);
        if(logging::file_logger("master", session_log_dir, logging::options)) {
            LOG_INFO("Session log directory: {}", session_log_dir);
        }
    }
    for(auto& project: projects) {
        log_configuration(*project, init_options_json);
    }

    LOG_INFO("Server ready (projects={}, stateful={}, stateless={})",
             projects.size(),
             pool_opts.stateful_count,
             pool_opts.stateless_count);

    pool_opts.log_dir = session_log_dir;
    if(!pool.start(pool_opts)) {
        LOG_ANOMALY(WorkerSpawnFail, "Failed to start worker pool");
        return;
    }

    lifecycle = ServerLifecycle::Ready;

    wire();

    rehome_sessions(*projects.front());
    for(auto& project: projects) {
        project->start();
    }
}

void MasterServer::initialize(llvm::StringRef root) {
    workspace_roots = {root.str()};
    initialize();
}

void MasterServer::wire() {
    pool.on_crash = [this](const WorkerCrashInfo& info) {
        // A stateless crash loses only in-flight requests, which fail back
        // to their callers with dispatch_errc::worker_crashed — the families
        // resend idempotent builds, the pump requeues the file. No state
        // outlives the request, so there is nothing to invalidate and no
        // event to dispatch.
        if(!info.stateful)
            return;
        llvm::DenseMap<ProjectServer*, llvm::SmallVector<Fid>> lost;
        for(auto id: info.lost_documents) {
            lost[&owner_of(Fid{id})].push_back(Fid{id});
        }
        for(auto& [project, documents]: lost) {
            project->dispatch(FileEvent::worker_crashed(documents));
        }
    };

    pool.on_evicted = [this](const std::string& path, std::size_t worker_index) {
        auto id = files.find(path);
        if(!id) {
            LOG_WARN("Evicted path not in pool: {}", path);
            return;
        }
        // Owner-table upkeep is pool-domain state and stays here; the
        // session-side consequence (the worker's AST is gone, same as a
        // crash) goes through the event pipeline like any invalidation.
        // Only the current owner's eviction counts: a stale copy left
        // behind by a probe reassignment says nothing about the document
        // the new owner still holds.
        if(pool.remove_owner_from(id->raw, worker_index)) {
            owner_of(*id).dispatch(FileEvent::document_evicted(*id));
        } else {
            LOG_INFO("Ignoring eviction of {} from non-owner worker {}", path, worker_index);
        }
    };
}

ProjectServer* MasterServer::claimant(Fid path_id) {
    for(auto& project: projects) {
        if(!project->project.build.commands(path_id).empty()) {
            return project.get();
        }
    }
    for(auto& project: projects) {
        if(!project->project.dep_graph.get_includers(path_id).empty()) {
            return project.get();
        }
    }
    auto path = files.resolve(path_id);
    ProjectServer* deepest = nullptr;
    for(auto& project: projects) {
        if(!project->root.empty() && path::under(path, project->root) &&
           (!deepest || project->root.size() > deepest->root.size())) {
            deepest = project.get();
        }
    }
    return deepest;
}

ProjectServer& MasterServer::route(Fid path_id) {
    auto* project = claimant(path_id);
    return project ? *project : *projects.front();
}

ProjectServer& MasterServer::owner_of(Fid path_id) {
    if(auto it = owners.find(path_id); it != owners.end()) {
        return *it->second;
    }
    return route(path_id);
}

std::shared_ptr<Session> MasterServer::find_session(Fid path_id) {
    auto it = owners.find(path_id);
    return it != owners.end() ? it->second->sessions.find(path_id) : nullptr;
}

std::shared_ptr<Session> MasterServer::open_session(Fid path_id) {
    if(lifecycle == ServerLifecycle::Ready && !owners.contains(path_id) && !claimant(path_id)) {
        if(auto root = project_root_above(path::parent_path(files.resolve(path_id)));
           !root.empty()) {
            add_folder(std::move(root));
        }
    }
    auto& project = owner_of(path_id);
    owners[path_id] = &project;
    return project.open_session(path_id);
}

void MasterServer::close_session(Fid path_id) {
    auto it = owners.find(path_id);
    if(it == owners.end()) {
        return;
    }
    auto* project = it->second;
    owners.erase(it);
    project->close_session(path_id);
}

void MasterServer::discover_around(Fid path_id) {
    auto path = files.resolve(path_id);
    for(auto& project: projects) {
        if(!project->root.empty() && path::under(path, project->root)) {
            project->discover_around(path_id);
        }
    }
}

void MasterServer::rehome_sessions(ProjectServer& from) {
    llvm::SmallVector<std::shared_ptr<Session>> leaving;
    for(auto& [path_id, session]: from.sessions.sessions) {
        if(session && &route(path_id) != &from) {
            leaving.push_back(session);
        }
    }
    for(auto& session: leaving) {
        auto path_id = session->path_id;
        from.release_session(path_id);
        auto& to = route(path_id);
        owners[path_id] = &to;
        to.adopt_session(path_id, session->text, session->version);
    }
}

void MasterServer::add_folder(std::string root) {
    path::canonicalize(root);
    if(llvm::any_of(projects, [&](auto& project) { return project->root == root; })) {
        return;
    }
    LOG_INFO("Serving folder {}", root);
    std::vector<std::string> cache_dirs;
    for(auto& project: projects) {
        cache_dirs.push_back(project->project.config.project.cache_dir);
    }
    auto& added = *projects.emplace_back(std::make_unique<ProjectServer>(*this, root));
    added.configure(init_options_json, cache_dirs);
    added.start();
    // Documents another project served until now may belong here: a
    // database under the new root lists them, or they sit inside it.
    for(auto& project: projects) {
        if(project.get() != &added) {
            rehome_sessions(*project);
        }
    }
}

void MasterServer::remove_folder(llvm::StringRef root) {
    std::string canonical = root.str();
    path::canonicalize(canonical);
    auto it = llvm::find_if(projects, [&](auto& project) { return project->root == canonical; });
    if(it == projects.end()) {
        return;
    }
    LOG_INFO("No longer serving folder {}", canonical);
    std::unique_ptr<ProjectServer> removed = std::move(*it);
    projects.erase(it);
    if(projects.empty()) {
        projects.push_back(std::make_unique<ProjectServer>(*this, std::string()));
        projects.front()->configure(init_options_json, {});
        projects.front()->start();
    }
    for(auto& [path_id, session]: removed->sessions.sessions) {
        owners.erase(path_id);
    }
    rehome_sessions(*removed);
    bg_tasks.spawn([](std::unique_ptr<ProjectServer> project) -> kota::task<> {
        co_await project->shutdown();
        project->close();
    }(std::move(removed)));
}

IndexPump::Progress MasterServer::index_progress() const {
    using Stage = IndexPump::Progress::Stage;
    IndexPump::Progress all{.stage = Stage::End};
    for(auto& project: projects) {
        auto& one = project->sched.pump.progress();
        if(one.stage == Stage::End) {
            continue;
        }
        all.total += one.total;
        all.completed += one.completed;
        all.dispatched += one.dispatched;
        all.stage =
            one.stage == Stage::Begin && all.stage == Stage::End ? Stage::Begin : Stage::Report;
    }
    return all;
}

std::vector<protocol::SymbolInformation> MasterServer::workspace_symbol(llvm::StringRef query) {
    constexpr std::size_t limit = 100;
    llvm::SmallVector<std::vector<protocol::SymbolInformation>> ranked;
    for(auto& project: projects) {
        ranked.push_back(project->features.workspace_symbol(query));
    }
    std::vector<protocol::SymbolInformation> merged;
    std::set<std::tuple<std::string, std::uint32_t, std::uint32_t, std::string>> seen;
    for(std::size_t rank = 0; merged.size() < limit; rank += 1) {
        bool any = false;
        for(auto& list: ranked) {
            if(rank >= list.size() || merged.size() == limit) {
                continue;
            }
            any = true;
            auto& info = list[rank];
            auto& start = info.location.range.start;
            if(seen.emplace(info.location.uri, start.line, start.character, info.name).second) {
                merged.push_back(std::move(info));
            }
        }
        if(!any) {
            break;
        }
    }
    return merged;
}

void MasterServer::schedule_shutdown() {
    if(lifecycle == ServerLifecycle::Exited)
        return;
    lifecycle = ServerLifecycle::ShuttingDown;
    shutdown_source.cancel();
}

kota::task<> MasterServer::shutdown_and_cleanup() {
    co_await bg_tasks.join();
    for(auto& project: projects) {
        co_await project->shutdown();
    }
    co_await pool.stop();
    for(auto& project: projects) {
        project->close();
    }
    lifecycle = ServerLifecycle::Exited;
}

struct Connection {
    std::unique_ptr<kota::ipc::JsonPeer> peer;
    std::unique_ptr<LSPClient> lsp_client;
};

static kota::task<> run_connection(kota::ipc::JsonPeer* peer,
                                   std::list<Connection>& connections,
                                   std::list<Connection>::iterator pos) {
    co_await peer->run();
    LOG_INFO("Client disconnected");
    connections.erase(pos);
}

/// Socket-mode serving body: the first connection gets the LSP slot,
/// later ones only a peer (the slot is never reclaimed).
static kota::task<> accept_connections(MasterServer& server,
                                       kota::tcp::acceptor acceptor,
                                       std::list<Connection>& connections) {
    auto& loop = kota::event_loop::current();
    kota::task_group<> group(loop);
    bool lsp_registered = false;

    group.spawn([](MasterServer& server,
                   kota::tcp::acceptor& acceptor,
                   std::list<Connection>& connections,
                   kota::task_group<>& group,
                   bool& lsp_registered) -> kota::task<> {
        auto& loop = kota::event_loop::current();

        while(true) {
            auto conn = co_await acceptor.accept();
            if(!conn.has_value())
                break;

            LOG_INFO("Client connected");

            auto transport = std::make_unique<kota::ipc::StreamTransport>(std::move(*conn));
            auto peer = std::make_unique<kota::ipc::JsonPeer>(loop, std::move(transport));

            std::unique_ptr<LSPClient> lsp;
            if(!lsp_registered) {
                lsp = std::make_unique<LSPClient>(server, *peer);
                lsp_registered = true;
            }

            auto* peer_ptr = peer.get();
            auto it = connections.emplace(connections.end(),
                                          Connection{
                                              .peer = std::move(peer),
                                              .lsp_client = std::move(lsp),
                                          });

            group.spawn(run_connection(peer_ptr, connections, it));
        }
    }(server, acceptor, connections, group, lsp_registered));

    co_await group.join();
}

int run_serve_mode(const ServerOptions& opts, const char* self_path) {
    logging::stderr_logger("master", logging::options);

    auto mode = opts.mode.value_or(ServerMode::Pipe);
    auto host = opts.host.value_or("127.0.0.1");
    auto port = opts.port.value_or(0);
    auto record = opts.record.value_or("");
    auto ws = opts.workspace.value_or("");

    LOG_INFO("clice master starting: version={}, target={}, pid={}, mode={}, workspace={}",
             clice::version,
             clice::target,
             llvm::sys::Process::getProcessId(),
             mode == ServerMode::Pipe ? "pipe" : "socket",
             ws.empty() ? "<from LSP initialize>" : ws);

    if(mode == ServerMode::Socket && (port <= 0 || port > 65535)) {
        LOG_ERROR("--port must be between 1 and 65535 in socket mode");
        return 1;
    }

    kota::event_loop loop;
    MasterServer server(loop, self_path, opts.configuration.value_or(""));

    if(mode == ServerMode::Pipe) {
        auto transport = kota::ipc::StreamTransport::open_stdio(loop);
        if(!transport) {
            LOG_ERROR("failed to open stdio transport");
            return 1;
        }

        std::unique_ptr<kota::ipc::Transport> final_transport = std::move(*transport);
        if(!record.empty()) {
            final_transport =
                std::make_unique<kota::ipc::RecordingTransport>(std::move(final_transport), record);
        }

        kota::ipc::JsonPeer lsp_peer(loop, std::move(final_transport));
        LSPClient lsp_client(server, lsp_peer);

        loop.schedule(
            [](MasterServer& server, kota::ipc::JsonPeer& peer, std::string root) -> kota::task<> {
                // Pre-initialize for standalone (no-editor) use; LSP initialize
                // will be rejected. Runs inside the loop — before the peer
                // reads its first message — because initialize() spawns
                // background tasks that need the running loop context.
                if(!root.empty()) {
                    server.initialize(root);
                }
                co_await kota::with_token(peer.run(), server.shutdown_token());
                co_await server.shutdown_and_cleanup();
            }(server, lsp_peer, ws));
        loop.run();
        return 0;
    }

    if(mode == ServerMode::Socket) {
        auto acceptor = kota::tcp::listen(host, port, {}, loop);
        if(!acceptor) {
            LOG_ERROR("failed to listen on {}:{}", host, port);
            return 1;
        }

        std::list<Connection> connections;
        LOG_INFO("Listening on {}:{} ...", host, port);
        loop.schedule([](MasterServer& server,
                         kota::tcp::acceptor acceptor,
                         std::list<Connection>& connections,
                         std::string root) -> kota::task<> {
            // See the pipe-mode comment: pre-initialization must run
            // inside the loop.
            if(!root.empty()) {
                server.initialize(root);
            }
            co_await kota::with_token(accept_connections(server, std::move(acceptor), connections),
                                      server.shutdown_token());
            co_await server.shutdown_and_cleanup();
        }(server, std::move(*acceptor), connections, ws));
        loop.run();
        return 0;
    }

    LOG_ERROR("unexpected server mode");
    return 1;
}

}  // namespace clice

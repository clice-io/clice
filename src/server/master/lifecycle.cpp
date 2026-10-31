module;

#include "version.h"

module clice;

import :settings.settings;
import :server.client;
import :server.master;
import :server.served_project;
import :support.anomaly;
import :support.environment;
import :support.logging;
import :support.process;
import :vfs.file_system;
import :worker.probe;

namespace clice {

/// Retention bound of the notify log. Subscribers drain promptly, so only
/// messages that fire before any client attaches accumulate (a handful of
/// startup guidance reports in practice); the cap is a safety net, not a
/// working-set size.
constexpr static std::size_t notify_log_limit = 128;

MasterServer::MasterServer(kota::event_loop& loop, std::string requested_configuration) :
    loop(loop), pool(loop), requested_configuration(std::move(requested_configuration)) {
    std::string unused;
    files.disk.shadow = sys::get_env(unused, "CLICE_SHADOW_FRESHNESS");
    // A disk change can be seen deep inside any operation — a staleness
    // check, a rescan inside a cascade: the changes apply on the next loop
    // turn, outside it, before any compile that read the change can
    // finish; applied later, they would mark that compile's AST stale.
    files.disk.on_change = [this] {
        bg_tasks.spawn([](MasterServer& server) -> kota::task<> {
            co_await kota::sleep(std::chrono::milliseconds(0));
            server.apply_disk_changes();
        }(*this));
    };
    polling.spawn(files.disk.end_turns(loop));
    // The notify hook is process-wide because the logging layer cannot
    // depend on the server; the composition root owns it for the server's
    // lifetime and turns reports into state (notify_log) the client is told
    // of. Master-side reports only ever fire on the event-loop thread (see
    // support/anomaly.cppm), so no synchronization is needed here.
    logging::set_notify_hook([this](logging::NotifyKind kind, std::string_view message) {
        notify_log.push_back(NotifyMessage{kind, std::string(message)});
        if(notify_log.size() > notify_log_limit) {
            notify_log.pop_front();
        }
        notify_seq += 1;
        if(client) {
            client->forward_notify_messages();
        }
    });
}

MasterServer::~MasterServer() {
    // The projects go first, while the members their release reads live.
    lifecycle = ServerLifecycle::Exited;
    projects.clear();
    // A server never shut down (a unit test) still ends the file table's
    // turns and its watch on the client.
    polling.cancel();
    client_watch.cancel();
    logging::set_notify_hook(nullptr);
}

void MasterServer::log_settings(const ServedProject& project) const {
    if(project.settings_file.empty()) {
        LOG_INFO("Configuration file: Missing (project {})", project.project.root);
    } else {
        auto text = vfs::read_text(project.settings_file);
        LOG_INFO("Configuration file {}:\n{}",
                 project.settings_file,
                 text.value_or("<unreadable>"));
    }
    if(!init_options) {
        LOG_INFO("initializationOptions: Missing");
    } else if(auto json = json::to_string(*init_options)) {
        auto pretty = json::prettify(*json);
        LOG_INFO("initializationOptions:\n{}", pretty ? *pretty : *json);
    }
    if(auto json = json::to_string(project.project.settings)) {
        auto pretty = json::prettify(*json);
        LOG_INFO("Effective configuration:\n{}", pretty ? *pretty : *json);
    }
}

void MasterServer::initialize() {
    for(auto& root: project_roots()) {
        projects.push_back(make_project(std::move(root)));
    }
    projects_generation += 1;
    watch_settings();
    for(auto& project: projects) {
        project->configure(init_options, taken_cache_dirs());
    }
    // One pool serves every project, sized for the most demanding one;
    // the first project names the log directory.
    WorkerPoolOptions pool_opts;
    pool_opts.stateful_count = 0;
    pool_opts.stateless_count = 0;
    pool_opts.min_stateless = 0;
    pool_opts.max_stateless = 0;
    bool unbounded = false;
    for(auto& project: projects) {
        auto& cfg = project->project.settings.project;
        pool_opts.stateful_count =
            std::max(pool_opts.stateful_count, cfg.stateful_worker_count.value);
        pool_opts.stateless_count =
            std::max(pool_opts.stateless_count, cfg.stateless_worker_count.value);
        pool_opts.min_stateless =
            std::max(pool_opts.min_stateless, cfg.min_stateless_worker_count.value);
        pool_opts.max_stateless =
            std::max(pool_opts.max_stateless, cfg.max_stateless_worker_count.value);
        // 0 leaves the ceiling to the core count, the largest one.
        unbounded = unbounded || cfg.max_stateless_worker_count.value == 0;
    }
    if(unbounded) {
        pool_opts.max_stateless = 0;
    }
    // Let integration tests hang a worker without waiting out the real
    // deadline, and drive eviction through a real server.
    if(auto ms = env_integer("CLICE_TEST_REQUEST_DEADLINE_MS")) {
        pool_opts.build_deadline = pool_opts.query_deadline = pool_opts.stall_deadline =
            std::chrono::milliseconds(*ms);
    }
    pool_opts.max_documents = env_integer("CLICE_TEST_MAX_DOCUMENTS");

    auto& first = projects.front()->project.settings.project;
    if(!first.logging_dir.empty()) {
        auto dir = logging::session_log_directory(first.logging_dir);
        if(logging::file_logger("master", dir, logging::options)) {
            LOG_INFO("Session log directory: {}", dir);
            pool_opts.log_dir = std::move(dir);
        }
    }
    for(auto& project: projects) {
        log_settings(*project);
    }

    LOG_INFO("Server ready (projects={}, stateful={}, stateless={})",
             projects.size(),
             pool_opts.stateful_count,
             pool_opts.stateless_count);

    if(projects.front()->project.settings.project.test_hooks.value) {
        pool.probe = &probe;
    }
    if(pool.start(pool_opts)) {
        lifecycle = ServerLifecycle::Ready;
        route_pool_callbacks();
        for(auto& project: projects) {
            project->start();
        }
        polling.spawn(poll_task());
    } else {
        LOG_ANOMALY(WorkerSpawnFail, "Failed to start worker pool");
    }
    // The documents opened before wait for their owners.
    for(auto& [path_id, document]: documents) {
        discover_around(path_id);
        auto& owner = route(path_id);
        document.owner = &owner;
        owner.adopt(document.session);
    }
    if(client) {
        client->publish_settings_diagnostics();
    }
}

void MasterServer::route_pool_callbacks() {
    pool.on_crash = [this](const WorkerCrashInfo& info) {
        // A stateless crash loses only in-flight requests, which fail back
        // to their senders (see deliver). No state outlives the request, so
        // there is nothing to invalidate.
        if(!info.stateful)
            return;
        llvm::DenseMap<ServedProject*, llvm::SmallVector<Fid>> lost;
        for(auto id: info.lost_documents) {
            if(auto* document = find_document(Fid{id}); document && document->owner) {
                lost[document->owner].push_back(Fid{id});
            }
        }
        for(auto& [project, documents]: lost) {
            project->asts_lost(documents);
        }
    };

    pool.on_evicted = [this](const std::string& path, std::size_t worker_index) {
        auto id = files.find(Spelling::absolute(path));
        assert(id && "an evicted document the master never sent");
        // Owner-table upkeep is pool-domain state and stays here; the
        // document's side of it (the worker's AST is gone, same as a crash)
        // goes to its owner. A live round's compile puts the document back
        // on its owner — the worker evicted it before that compile arrived,
        // or it would have kept it — so the eviction changes nothing. One
        // that crossed the compile's reply over IPC leaves the master
        // trusting a document the worker no longer holds: the next query
        // hears document_unloaded and compiles it again (see ast_request).
        auto* document = find_document(*id);
        auto* project = document ? document->owner : nullptr;
        if(project && project->compiling(*id)) {
            LOG_INFO("Ignoring eviction of {}: a compile of it is under way", path);
            return;
        }
        // Only the current owner's eviction counts: a copy left behind on
        // a worker that lost ownership says nothing about the document the
        // new owner still holds.
        if(pool.remove_owner_from(id->raw, worker_index)) {
            if(project) {
                project->asts_lost({*id});
            }
        } else {
            LOG_INFO("Ignoring eviction of {} from non-owner worker {}", path, worker_index);
        }
    };
}

void MasterServer::schedule_shutdown() {
    if(lifecycle == ServerLifecycle::Exited)
        return;
    lifecycle = ServerLifecycle::ShuttingDown;
    shutdown_source.cancel();
}

bool MasterServer::test_hooks() const {
    return pool.probe != nullptr;
}

feature::SemanticTokensOptions MasterServer::startup_token_options() const {
    return load_settings(project_roots().front(), {.overlay = init_options, .strict = false})
        .settings.semantic_tokens;
}

kota::task<> MasterServer::stop() {
    if(!stop_started) {
        stop_started = true;
        stop_run.spawn([](MasterServer& server) -> kota::task<> {
            // A client that went away skipped the shutdown request; no
            // project may start from here on (a project stop finishing below
            // would serve the folders again, see make_project).
            server.lifecycle = ServerLifecycle::ShuttingDown;
            co_await server.pool.stop();
            server.polling.cancel();
            co_await server.polling.join();
            co_await server.bg_tasks.join();
            for(auto& project: server.projects) {
                co_await project->shutdown();
            }
            server.stopped.set();
        }(*this));
    }
    co_await stopped.wait();
}

kota::task<> MasterServer::close() {
    co_await stop();
    co_await stop_run.join();
    client_watch.cancel();
    co_await client_watch.join();
    for(auto& project: projects) {
        project->sched.close();
    }
    lifecycle = ServerLifecycle::Exited;
}

void MasterServer::watch_client(std::uint32_t pid) {
    client_watch.spawn([](MasterServer& server, std::uint32_t pid) -> kota::task<> {
        while(sys::process_alive(static_cast<int>(pid))) {
            // No event: the editor is no child of the server's to wait for.
            co_await kota::sleep(std::chrono::seconds(2), server.loop);
        }
        LOG_INFO("Client process {} exited; shutting down", pid);
        server.schedule_shutdown();
    }(*this, pid));
}

}  // namespace clice

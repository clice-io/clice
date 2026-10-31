module clice;

import :feature.protocol;
import :project.save;
import :server.client;
import :server.extension;
import :server.master;
import :server.served_project;
import :support.logging;
import :worker.probe;

namespace clice {

static ipc::Error hooks_off() {
    return ipc::Error{protocol::ErrorCode::InvalidRequest, "test hooks are not enabled"};
}

/// Look at the disk and the databases of every project, as
/// clice/internal/poll asks, after a look at the configuration files.
/// Returns the changes found.
static async::task<std::uint32_t> poll_projects(MasterServer& srv, const ext::PollParams& params) {
    srv.look_at_settings();
    bool cdb = params.loop == "cdb";
    std::uint32_t count = 0;
    if(cdb) {
        srv.files.disk.look_flags();
    } else {
        srv.files.disk.look_all();
        // Before the sources walk suspends: the changes the looks queued
        // would be applied uncounted.
        count += static_cast<std::uint32_t>(srv.apply_disk_changes());
    }
    // Copied: a project may come or go while the sources are walked.
    auto projects = srv.projects;
    for(auto& project: projects) {
        if(cdb) {
            count +=
                static_cast<std::uint32_t>(project->reload_databases(params.force.value_or(true)));
            continue;
        }
        if(co_await project->poll_sources()) {
            count += 1;
        }
    }
    co_return count;
}

/// The file a hold or gate is placed on, in the spelling the worker
/// requests carry it under. Refused once the server is shutting down: the
/// stopping pool released every one, and a later one would park work the
/// shutdown waits on.
static std::expected<std::string, ipc::Error> park_target(MasterServer& srv, const Spelling& path) {
    if(srv.shutting_down()) {
        return std::unexpected(
            ipc::Error{protocol::ErrorCode::InvalidRequest, "the server is shutting down"});
    }
    return std::string(srv.files.resolve(srv.files.intern(path)));
}

void LSPClient::register_test_hooks() {
    // Looks at the disk once (see ext::PollParams).
    // Test-only and not a stable API.
    on_request<ext::PollParams>(
        "clice/internal/poll",
        [this](RequestContext& ctx, const ext::PollParams& params) -> RawResult {
            auto& srv = this->server;
            if(params.loop != "cdb" && params.loop != "workspace") {
                co_await async::fail(ipc::Error{protocol::ErrorCode::InvalidParams,
                                                R"(loop must be "cdb" or "workspace")"});
            }
            co_return to_raw(ext::PollResult{co_await poll_projects(srv, params)});
        });

    on_request<ext::SyncParams>(
        "clice/internal/sync",
        [this](RequestContext& ctx, const ext::SyncParams& params) -> RawResult {
            auto& srv = this->server;
            if(!srv.test_hooks()) {
                co_await async::fail(hooks_off());
            }
            // The deadline counts from the request; a poll runs to its end,
            // one cut short would leave the databases out of step.
            auto deadline = std::chrono::steady_clock::now() +
                            std::chrono::milliseconds(params.deadline_ms.value_or(240'000));
            if(params.poll.value_or(false)) {
                co_await poll_projects(srv, {.loop = "workspace"});
            }
            auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::max(deadline - std::chrono::steady_clock::now(),
                         std::chrono::steady_clock::duration::zero()));
            // Hang detection: past the deadline, the answer names what
            // still runs.
            co_await async::when_any(srv.wait_idle(this->peer), async::sleep(left, srv.loop));
            ext::SyncResult result;
            for(auto& project: srv.projects) {
                for(auto id: project->sched.pump.failed()) {
                    result.failed.push_back(feature::to_uri(srv.files.display(id)));
                }
                result.unsaved = result.unsaved || has_unsaved_state(project->project);
            }
            ranges::sort(result.failed);
            result.pending = srv.pending_work(this->peer);
            co_return to_raw(result);
        });

    on_request<ext::HoldParams>(
        "clice/internal/hold",
        [this](RequestContext& ctx, const ext::HoldParams& params) -> RawResult {
            auto& srv = this->server;
            if(!srv.test_hooks()) {
                co_await async::fail(hooks_off());
            }
            auto kind = parse_build_kind(params.kind);
            auto path = uri_to_path(params.uri);
            if(!kind || !path) {
                co_await async::fail(ipc::Error{
                    protocol::ErrorCode::InvalidParams,
                    R"(a hold names a kind of "compile", "pch", "pcm" or "index", and a file URI)"});
            }
            auto file = park_target(srv, *path);
            if(!file) {
                co_await async::fail(std::move(file.error()));
            }
            co_return to_raw(ext::HoldResult{srv.probe.hold(*kind, std::move(*file))});
        });

    on_request<ext::GateParams>(
        "clice/internal/gate",
        [this](RequestContext& ctx, const ext::GateParams& params) -> RawResult {
            auto& srv = this->server;
            if(!srv.test_hooks()) {
                co_await async::fail(hooks_off());
            }
            auto path = uri_to_path(params.uri);
            if(params.request.empty() || !path) {
                co_await async::fail(ipc::Error{protocol::ErrorCode::InvalidParams,
                                                "a gate names a worker request and a file URI"});
            }
            auto file = park_target(srv, *path);
            if(!file) {
                co_await async::fail(std::move(file.error()));
            }
            co_return to_raw(ext::HoldResult{
                srv.probe.gate(std::format("clice/worker/{} {}", params.request, *file))});
        });

    on_request<ext::ReleaseParams>(
        "clice/internal/release",
        [this](RequestContext& ctx, const ext::ReleaseParams& params) -> RawResult {
            auto& srv = this->server;
            if(!srv.test_hooks()) {
                co_await async::fail(hooks_off());
            }
            if(!srv.probe.release(params.id)) {
                co_await async::fail(ipc::Error{protocol::ErrorCode::InvalidParams,
                                                std::format("no hold {}", params.id)});
            }
            co_return to_raw(ext::ReleaseResult{});
        });

    on_request<ext::WatchParams>(
        "clice/internal/watch",
        [this](RequestContext& ctx, const ext::WatchParams& params) -> RawResult {
            auto& srv = this->server;
            if(!srv.test_hooks()) {
                co_await async::fail(hooks_off());
            }
            auto event = parse_loop_event(params.event);
            if(!event) {
                co_await async::fail(
                    ipc::Error{protocol::ErrorCode::InvalidParams,
                               R"(a watch names "databaseChange", "diskChange" or "checkpoint")"});
            }
            co_return to_raw(ext::HoldResult{srv.probe.watch(*event)});
        });

    on_request<ext::LogFloodParams>(
        "clice/internal/logFlood",
        [this](RequestContext& ctx, const ext::LogFloodParams& params) -> RawResult {
            // Load-generating hook: a stray client must not be able to
            // bloat the file log.
            if(!this->server.test_hooks()) {
                co_await async::fail(hooks_off());
            }
            auto count = std::min<std::uint32_t>(params.count, 100'000);
            auto size = std::clamp<std::uint32_t>(params.size, 16, 4096);
            std::string padding(size, 'f');
            for(std::uint32_t i = 0; i < count; ++i) {
                LOG_INFO("[stderr-flood {}] {}", i, padding);
            }
            co_return to_raw(ext::LogFloodResult{count});
        });

    // Ownership gauges for memory-lifecycle tests (see ext::StatsParams).
    // Read-only and synchronous: every counter is computed from live
    // master state on the event loop, so an assertion made after a finished
    // operation observes exactly the state that operation left behind.
    on_request<ext::StatsParams>(
        "clice/internal/stats",
        [this](RequestContext& ctx, const ext::StatsParams&) -> RawResult {
            ext::StatsResult stats;
            for(auto& served: this->server.projects) {
                auto& project = served->project;
                for(auto& entry: project.pch_cache) {
                    auto& st = entry.second;
                    if(st.state) {
                        stats.pch_loaded_states += 1;
                        stats.pch_state_bytes += st.state->bytes().size();
                    }
                }
                stats.pch_cache_entries += static_cast<std::uint32_t>(project.pch_cache.size());
                stats.pch_stashed_bytes += served->sched.pch.stashed_bytes();

                stats.index_inmemory_shards += project.index.pending_shard_writes();
                stats.index_shard_content_bytes += project.index.shard_bytes();
                stats.last_save_shards += project.index.last_save_shards();

                if(project.store) {
                    stats.pending_tmp_files +=
                        static_cast<std::uint32_t>(project.store->pending_tmp_files());
                }

                project.hosting.contexts.for_each([&](Fid, const auto& entry) {
                    stats.header_contexts += 1;
                    stats.synthesized_contexts += entry.result.synthesized != nullptr ? 1 : 0;
                });

                stats.import_scans += served->sched.pcm.import_scans;
                stats.index_idle = stats.index_idle && served->sched.pump.is_idle();
            }
            stats.sessions = static_cast<std::uint32_t>(this->server.documents.size());
            stats.workers = {.stateful = this->server.pool.worker_pids(true),
                             .stateless = this->server.pool.worker_pids(false)};
            stats.checks_looked = this->server.files.disk.checks.looked;
            stats.checks_trusted = this->server.files.disk.checks.trusted;
            for(auto& entry: this->server.probe.builds) {
                auto& counts = entry.getValue();
                stats.builds.push_back({
                    .uri = feature::to_uri(this->server.files.display(
                        this->server.files.intern(Spelling::absolute(entry.getKey())))),
                    .compile = counts[std::to_underlying(BuildKind::Compile)],
                    .pch = counts[std::to_underlying(BuildKind::PCH)],
                    .pcm = counts[std::to_underlying(BuildKind::PCM)],
                    .index = counts[std::to_underlying(BuildKind::Index)],
                });
            }
            co_return to_raw(stats);
        });
}

}  // namespace clice

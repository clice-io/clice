#include "server/index/background_indexer.h"

#include <algorithm>
#include <cassert>
#include <format>
#include <optional>
#include <string>
#include <vector>

#include "index/tu_index.h"
#include "server/context/context_resolver.h"
#include "server/protocol/worker.h"
#include "server/session/session_store.h"
#include "server/worker/worker_pool.h"
#include "support/filesystem.h"
#include "support/logging.h"
#include "support/timer.h"

#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/xxhash.h"

namespace clice {

void BackgroundIndexer::merge(const void* tu_index_data, std::size_t size) {
    auto tu_index = index::TUIndex::from(tu_index_data);
    if(tu_index.graph.paths.empty()) {
        LOG_WARN("Ignoring TUIndex with empty path graph");
        return;
    }
    auto file_ids_map = workspace.project_index.merge(tu_index, workspace.path_pool);
    auto main_tu_path_id = static_cast<std::uint32_t>(tu_index.graph.paths.size() - 1);
    llvm::StringRef main_tu_path = tu_index.graph.paths[main_tu_path_id];

    // Collect non-External symbols referenced in a FileIndex.  Each file's
    // MergedIndex shard stores exactly the local symbols its occurrences
    // reference, so lookup is a single shard check — no scanning.
    auto collect_local_symbols = [&](const index::FileIndex& file_idx) {
        index::SymbolTable result;
        for(auto& occ: file_idx.occurrences) {
            auto it = tu_index.symbols.find(occ.target);
            if(it != tu_index.symbols.end() && it->second.scope != index::SymbolScope::External) {
                result.try_emplace(occ.target, it->second);
            }
        }
        return result;
    };

    // Only shards that actually receive this TU's new contribution count as
    // touched; a file still in the include graph but with an empty FileIndex
    // must be swept below like a dropped one.
    llvm::DenseSet<std::uint32_t> touched;

    auto merge_file_index = [&](std::uint32_t tu_path_id, index::FileIndex& file_idx) {
        auto global_path_id = file_ids_map[tu_path_id];
        auto& shard = workspace.merged_indices[global_path_id];

        if(tu_path_id == main_tu_path_id) {
            llvm::SmallVector<index::DepLocation> deps;
            deps.reserve(tu_index.graph.locations.size() + 1);
            // The TU's own content is a dependency of its shard too: without
            // it, a closed TU edited on disk with unchanged includes would
            // never look stale.
            deps.push_back({main_tu_path, 0, 0});
            for(auto& loc: tu_index.graph.locations) {
                deps.push_back({tu_index.graph.paths[loc.path_id], loc.line, loc.include});
            }
            auto file_path = workspace.path_pool.resolve(global_path_id);
            auto buf = llvm::MemoryBuffer::getFile(file_path);
            if(!buf) {
                // Overwriting the shard's stored content with nothing would
                // silently break every position mapping for this TU; keep the
                // old snapshot, the staleness check re-enqueues it later.
                LOG_WARN("Skip merge for {}: cannot read content: {}",
                         file_path,
                         buf.getError().message());
                return;
            }
            shard.merge(main_tu_path, tu_index.built_at, deps, file_idx, (*buf)->getBuffer());
            shard.merge_symbols(collect_local_symbols(file_idx));
            touched.insert(global_path_id);
        } else {
            std::optional<std::uint32_t> include_id;
            for(std::uint32_t i = 0; i < tu_index.graph.locations.size(); ++i) {
                if(tu_index.graph.locations[i].path_id == tu_path_id) {
                    include_id = i;
                    break;
                }
            }
            if(!include_id) {
                LOG_WARN("Skip merge for path {}: include location not found", global_path_id);
                return;
            }
            auto header_path = workspace.path_pool.resolve(global_path_id);
            llvm::StringRef header_content;
            std::string header_content_storage;
            auto header_buf = llvm::MemoryBuffer::getFile(header_path);
            if(header_buf) {
                header_content_storage = (*header_buf)->getBuffer().str();
                header_content = header_content_storage;
            }
            // Keyed by the including TU so a reindex of that TU replaces its
            // prior contribution to this header's shard.
            shard.merge(main_tu_path, *include_id, file_idx, header_content);
            shard.merge_symbols(collect_local_symbols(file_idx));
            touched.insert(global_path_id);
        }
    };

    for(auto& [tu_path_id, file_idx]: tu_index.path_file_indices) {
        merge_file_index(tu_path_id, file_idx);
    }
    merge_file_index(main_tu_path_id, tu_index.main_file_index);

    // A file dropped from this TU (a removed transitive include, or one
    // whose contribution became empty) is no longer merged above, but its
    // shard may still hold this TU's previous contribution — sweep it, or
    // references under the dropped include keep being served as if the edge
    // still existed.
    for(auto& [path_id, shard]: workspace.merged_indices) {
        if(touched.contains(path_id)) {
            continue;
        }
        if(shard.has_contribution(main_tu_path)) {
            shard.remove(main_tu_path);
        }
    }

    auto external_count = std::ranges::count_if(tu_index.symbols, [](auto& kv) {
        return kv.second.scope == index::SymbolScope::External;
    });
    LOG_INFO("Merged TUIndex: {} paths, {} symbols ({} external), {} merged_shards",
             tu_index.graph.paths.size(),
             tu_index.symbols.size(),
             external_count,
             workspace.merged_indices.size());
}

/// Stable blob key for a file's shard: runtime pool ids are per-session,
/// so blobs are named by a hash of the path instead.
static std::string shard_key(llvm::StringRef path) {
    return std::format("{:016x}", llvm::xxh3_64bits(path));
}

/// Begin a two-phase store write and serialize the blob to its tmp path.
/// Returns the entry to commit, or nullopt if serialization failed.
static std::optional<CacheStore::PendingEntry>
    serialize_blob(CacheStore& store,
                   llvm::StringRef key,
                   llvm::function_ref<void(llvm::raw_ostream&)> serialize) {
    auto pending = store.begin_store("index", key);
    std::error_code ec;
    llvm::raw_fd_ostream os(pending.tmp_path, ec);
    if(ec) {
        LOG_WARN("Failed to write index blob {}: {}", key, ec.message());
        store.abort(pending);
        return std::nullopt;
    }
    serialize(os);
    os.flush();
    // A truncated blob (disk full) must never be committed: the index
    // namespace is Persistent, so it would be served forever.
    if(os.has_error()) {
        LOG_WARN("Failed to write index blob {}: {}", key, os.error().message());
        os.clear_error();
        store.abort(pending);
        return std::nullopt;
    }
    return pending;
}

kota::task<> BackgroundIndexer::save() {
    if(!workspace.store)
        co_return;
    auto& store = *workspace.store;
    ScopedTimer timer;

    // Phase 1, synchronous: serialize the ProjectIndex and every dirty
    // shard to tmp files.  No suspension point in between, so the batch is
    // a consistent snapshot even if a merge runs before the commits below
    // are done.  Shards are only published together with the ProjectIndex
    // they were built against: pairing new shards with an old project blob
    // (or vice versa) would serve a mixed snapshot after restart.
    // The project blob doubles as the shard manifest: it lists every file
    // owning a shard so the loader knows exactly which blobs to fetch (and
    // sweeps the rest).
    llvm::SmallVector<std::uint32_t> shard_ids;
    shard_ids.reserve(workspace.merged_indices.size());
    for(auto& [path_id, shard]: workspace.merged_indices) {
        shard_ids.push_back(path_id);
    }

    auto project_pending = serialize_blob(store, "project", [&](llvm::raw_ostream& os) {
        workspace.project_index.serialize(os, workspace.path_pool, shard_ids);
    });
    if(!project_pending) {
        LOG_WARN("Skipping index save: ProjectIndex serialization failed");
        co_return;
    }
    LOG_INFO("Saved ProjectIndex ({} symbols)", workspace.project_index.symbols.size());

    llvm::SmallVector<CacheStore::PendingEntry> shards;
    std::size_t total = workspace.merged_indices.size();
    for(auto& [path_id, shard]: workspace.merged_indices) {
        if(!shard.need_rewrite())
            continue;
        if(auto pending = serialize_blob(store,
                                         shard_key(workspace.path_pool.resolve(path_id)),
                                         [&](llvm::raw_ostream& os) { shard.serialize(os); })) {
            shards.push_back(std::move(*pending));
        }
    }
    LOG_INFO("Saved {} MergedIndex shards (of {} total)", shards.size(), total);

    // Phase 2: commit each blob (fsync + atomic rename) on the kota thread
    // pool, keeping the heavy IO off the event loop.  The project blob goes
    // first; if it cannot be published, drop the shards of this snapshot.
    auto committed =
        co_await kota::queue([&] { return store.commit(std::move(*project_pending)); });
    if(!committed.has_value() || !committed.value().has_value()) {
        LOG_WARN("Failed to commit ProjectIndex blob, dropping {} shard blobs", shards.size());
        for(auto& pending: shards) {
            store.abort(pending);
        }
        co_return;
    }

    // FIXME: shard commits are strictly sequential (one co_await per shard).
    // For large projects this adds ~N×2ms of round-trip overhead.  Consider
    // batching commits or dispatching them in parallel on the thread pool.
    for(auto& pending: shards) {
        auto key = pending.key;
        auto result = co_await kota::queue([&] { return store.commit(std::move(pending)); });
        if(!result.has_value() || !result.value().has_value()) {
            LOG_WARN("Failed to commit index blob {}", key);
        }
    }

    LOG_PERF("index",
             "phase=save shards={} total={} elapsed_ms={}",
             shards.size(),
             total,
             timer.ms());
}

void BackgroundIndexer::load() {
    if(!workspace.store)
        return;
    ScopedTimer timer;

    bool has_project = false;
    llvm::StringSet<> expected_keys;
    auto project_path = workspace.store->lookup("index", "project");
    if(project_path) {
        auto buf = llvm::MemoryBuffer::getFile(*project_path);
        if(!buf) {
            // Transient read failure — don't load shards (useless without
            // the project index), but don't destroy them either.
            LOG_WARN("Failed to read ProjectIndex blob: {}", buf.getError().message());
            return;
        }
        // An unreadable or old-format blob loads as "no index on disk":
        // everything is swept and rebuilt once in the background.
        llvm::SmallVector<std::uint32_t> manifest;
        auto loaded = index::ProjectIndex::from((*buf)->getBufferStart(),
                                                (*buf)->getBufferSize(),
                                                workspace.path_pool,
                                                manifest);
        if(loaded) {
            workspace.project_index = std::move(*loaded);
            has_project = true;
            LOG_INFO("Loaded ProjectIndex: {} symbols", workspace.project_index.symbols.size());

            // The manifest names every shard blob; fetch exactly those. A
            // blob the loader rejects (corruption, old format) counts as
            // missing: enqueue the file so the background round rebuilds it
            // — for headers no CDB entry would ever re-enqueue it otherwise.
            for(auto path_id: manifest) {
                auto key = shard_key(workspace.path_pool.resolve(path_id));
                auto shard_path = workspace.store->lookup("index", key);
                auto shard =
                    shard_path ? index::MergedIndex::load(*shard_path) : index::MergedIndex();
                if(shard.loaded()) {
                    workspace.merged_indices[path_id] = std::move(shard);
                    expected_keys.insert(key);
                } else {
                    LOG_INFO("Discarding unreadable shard for {}",
                             workspace.path_pool.resolve(path_id));
                    enqueue(path_id);
                }
            }
        } else {
            LOG_INFO("Discarding old-format ProjectIndex blob");
        }
    }

    // Sweep every blob the manifest does not name (or all of them when the
    // project index itself is gone or outdated — Persistent namespace
    // cleanup is the caller's mark-and-sweep).
    llvm::SmallVector<std::string> orphans;
    workspace.store->for_each_key("index", [&](llvm::StringRef key) {
        if(key == "project" && has_project)
            return;
        if(!expected_keys.contains(key)) {
            orphans.push_back(key.str());
        }
    });

    for(auto& key: orphans) {
        workspace.store->invalidate("index", key);
    }

    if(!workspace.merged_indices.empty()) {
        LOG_INFO("Loaded {} MergedIndex shards", workspace.merged_indices.size());
    }
    LOG_PERF("startup",
             "phase=index_load symbols={} shards={} elapsed_ms={}",
             workspace.project_index.symbols.size(),
             workspace.merged_indices.size(),
             timer.ms());
}

bool BackgroundIndexer::need_update(llvm::StringRef file_path) {
    auto merged_it = workspace.merged_indices.find(workspace.path_pool.intern(file_path));
    if(merged_it == workspace.merged_indices.end())
        return true;

    return merged_it->second.need_update();
}

void BackgroundIndexer::enqueue(std::uint32_t server_path_id) {
    // Already queued and not yet consumed — a second entry would only be
    // skipped by need_update later; drop it here.
    if(!pending_ids.insert(server_path_id).second)
        return;
    index_queue.push_back(server_path_id);
}

void BackgroundIndexer::pause_indexing() {
    ++pause_depth;
    if(pause_depth == 1) {
        resume_event.reset();
        LOG_DEBUG("Background indexing paused");
    }
}

void BackgroundIndexer::resume_indexing() {
    if(pause_depth > 0)
        --pause_depth;
    if(pause_depth == 0) {
        resume_event.set();
        LOG_DEBUG("Background indexing resumed");
    }
}

kota::task<> BackgroundIndexer::stop() {
    bg_tasks.cancel();
    co_await bg_tasks.join();
}

void BackgroundIndexer::schedule() {
    if(!*workspace.config.project.enable_indexing || indexing_active || indexing_scheduled)
        return;
    indexing_scheduled = true;

    if(!index_idle_timer) {
        index_idle_timer = std::make_shared<kota::timer>(kota::timer::create(loop));
    }
    index_idle_timer->start(std::chrono::milliseconds(*workspace.config.project.idle_timeout_ms));

    if(!bg_tasks.spawn(run_background_indexing())) {
        indexing_scheduled = false;
        LOG_WARN("Failed to spawn background indexing task (task group stopped)");
    }
}

kota::task<> BackgroundIndexer::index_one(std::uint32_t server_path_id,
                                          std::size_t index,
                                          std::size_t total) {
    auto file_path = std::string(workspace.path_pool.resolve(server_path_id));

    if(sessions.find(server_path_id) != nullptr)
        co_return;

    if(!need_update(file_path))
        co_return;

    // For module interface units, compile their PCM (and transitive deps)
    // first so the stateless worker has the artifacts it needs.
    if(workspace.compile_graph && workspace.path_to_module.contains(server_path_id)) {
        co_await workspace.compile_graph->compile(server_path_id);
    }

    worker::BuildParams params;
    params.kind = worker::BuildKind::Index;
    params.file = file_path;
    // Bulk background indexing sticks to real commands; synthesized fallback
    // commands would fill the index with guesses.
    if(contexts.resolve_command(file_path, params.directory, params.arguments, nullptr) ==
       CommandSource::Fallback)
        co_return;

    workspace.fill_pcm_deps(params.pcms);

    LOG_INFO("[{}/{}] Indexing {}", index, total, file_path);

    ScopedTimer timer;
    auto result = co_await pool.send_stateless(params);
    if(result.has_value() && result.value().success && !result.value().tu_index_data.empty()) {
        auto index_ms = timer.ms();
        ScopedTimer merge_timer;
        merge(result.value().tu_index_data.data(), result.value().tu_index_data.size());
        LOG_PERF("index",
                 "progress={}/{} file={} bytes={} index_ms={} merge_ms={}",
                 index,
                 total,
                 file_path,
                 result.value().tu_index_data.size(),
                 index_ms,
                 merge_timer.ms());
    } else if(result.has_value() && !result.value().success) {
        LOG_WARN("[{}/{}] Index failed for {}: {}", index, total, file_path, result.value().error);
    } else if(result.has_value() && result.value().tu_index_data.empty()) {
        LOG_WARN("[{}/{}] Index returned empty TUIndex for {}", index, total, file_path);
    } else {
        LOG_WARN("[{}/{}] Index IPC error for {}: {}",
                 index,
                 total,
                 file_path,
                 result.error().message);
    }
}

kota::task<> BackgroundIndexer::run_background_indexing() {
    if(index_idle_timer) {
        co_await index_idle_timer->wait();
    }
    indexing_scheduled = false;

    if(index_queue_pos >= index_queue.size()) {
        LOG_DEBUG("Background indexing: queue exhausted");
        co_return;
    }

    indexing_active = true;
    LOG_DEBUG("Background indexing: starting, {} files queued",
              index_queue.size() - index_queue_pos);

    std::stable_partition(
        index_queue.begin() + index_queue_pos,
        index_queue.end(),
        [this](std::uint32_t id) { return workspace.path_to_module.contains(id); });

    auto total = index_queue.size() - index_queue_pos;
    std::size_t dispatched = 0;
    std::size_t completed = 0;

    // Announce the round; a progress reporter reads the counts from
    // progress() and owns the LSP token's begin/report/end handshake. With
    // no subscriber the signal is simply a no-op.
    progress_data = Progress{.stage = Progress::Stage::Begin, .total = total};
    on_progress_changed.emit();

    // Timed at the start of real work; the reporter's token handshake runs
    // off to the side and cannot inflate the reported indexing duration.
    ScopedTimer timer;
    kota::task_group<> workers(loop);

    while(index_queue_pos < index_queue.size()) {
        if(pause_depth > 0)
            co_await resume_event.wait();

        auto server_path_id = index_queue[index_queue_pos++];
        pending_ids.erase(server_path_id);
        auto file_path = std::string(workspace.path_pool.resolve(server_path_id));
        if(sessions.find(server_path_id) != nullptr || !need_update(file_path)) {
            ++completed;
            continue;
        }

        ++dispatched;
        workers.spawn([&, server_path_id, n = dispatched]() -> kota::task<> {
            co_await index_one(server_path_id, n, total);
            ++completed;
            progress_data.stage = Progress::Stage::Report;
            progress_data.completed = completed;
            on_progress_changed.emit();
        }());
    }

    LOG_DEBUG("Background indexing: all {} tasks spawned, waiting for completion", dispatched);
    co_await workers.join();

    // Skipped files bump `completed` without a Report emit; refresh the
    // materialized count so a subscriber waking up on End reads the truth.
    progress_data.completed = completed;
    progress_data.stage = Progress::Stage::End;
    progress_data.dispatched = dispatched;
    on_progress_changed.emit();

    indexing_active = false;

    // Safe point to compact: no dispatch loop holds an index into the queue.
    // Files enqueued while we awaited the workers keep the queue alive for
    // the next scheduled round.
    if(index_queue_pos >= index_queue.size()) {
        assert(pending_ids.empty() && "drained queue must have no pending ids");
        index_queue.clear();
        index_queue_pos = 0;
    }

    LOG_PERF("index",
             "phase=run dispatched={} skipped={} total={} elapsed_ms={}",
             dispatched,
             total - dispatched,
             total,
             timer.ms());
    co_await save();
}

}  // namespace clice

#include "server/compiler/indexer.h"

#include <algorithm>
#include <cassert>
#include <format>
#include <optional>
#include <string>
#include <vector>

#include "index/tu_index.h"
#include "server/compiler/context_resolver.h"
#include "server/protocol/worker.h"
#include "server/state/session_store.h"
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

void Indexer::merge(const void* tu_index_data, std::size_t size) {
    auto tu_index = index::TUIndex::from(tu_index_data);
    if(tu_index.graph.paths.empty()) {
        LOG_WARN("Ignoring TUIndex with empty path graph");
        return;
    }
    auto main_tu_path_id = static_cast<std::uint32_t>(tu_index.graph.paths.size() - 1);
    llvm::StringRef main_tu_path = tu_index.graph.paths[main_tu_path_id];

    // Shards pair the worker's rows with content read from disk here; if the
    // disk moved on since the worker read it, the rows' offsets describe
    // bytes that no longer exist and merging would misplace every position
    // until the next reindex. The worker's consumed-content hash arbitrates.
    // A missing hash (0) proceeds as before.
    auto content_matches = [&](std::uint32_t tu_path_id, llvm::StringRef disk_content) {
        auto consumed = tu_index.graph.path_hashes[tu_path_id];
        return consumed == 0 || llvm::xxh3_64bits(disk_content) == consumed;
    };

    // The main file's verdict gates the WHOLE result: its per-header shards
    // and the trailing sweep all describe this one compile, so applying any
    // of it against a moved-on main file would mix two generations (and the
    // sweep would strip contributions the stale result merely no longer
    // mentions). Skipping everything keeps the last-known state consistent;
    // the changed file fails the next staleness check (or is already
    // pending), and a follow-up pass redoes the merge against settled
    // content.
    auto main_buf = llvm::MemoryBuffer::getFile(main_tu_path);
    if(!main_buf) {
        LOG_WARN("Skip merge for {}: cannot read content: {}",
                 main_tu_path,
                 main_buf.getError().message());
        return;
    }
    if(!content_matches(main_tu_path_id, (*main_buf)->getBuffer())) {
        LOG_INFO("Skip merge for {}: disk moved on since it was indexed", main_tu_path);
        return;
    }

    auto file_ids_map = workspace.project_index.merge(tu_index, workspace.path_pool);

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
            auto& path_hashes = tu_index.graph.path_hashes;
            llvm::SmallVector<index::DepLocation> deps;
            deps.reserve(tu_index.graph.locations.size() + 1);
            // The TU's own content is a dependency of its shard too: without
            // it, a closed TU edited on disk with unchanged includes would
            // never look stale.
            deps.push_back({main_tu_path, 0, 0, path_hashes[main_tu_path_id]});
            for(auto& loc: tu_index.graph.locations) {
                deps.push_back({tu_index.graph.paths[loc.path_id],
                                loc.line,
                                loc.include,
                                path_hashes[loc.path_id]});
            }
            // Read and verified against the consumed hash before anything
            // was merged: an unreadable or moved-on main file skips this
            // whole result up front.
            shard.merge(main_tu_path, tu_index.built_at, deps, file_idx, (*main_buf)->getBuffer());
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
            // Unconditional, unlike the read above: an unreadable or
            // truncated-to-empty header must not slip past the arbitration
            // and pair the rows with content they were not built from.
            if(!content_matches(tu_path_id, header_content)) {
                LOG_INFO("Skip merge for {}: disk moved on since it was indexed", header_path);
                touched.insert(global_path_id);
                return;
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

kota::task<> Indexer::save() {
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

void Indexer::load() {
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
                    // No shard survives, so there is nothing stale to keep
                    // serving; ContentChanged states the truth ("the index
                    // does not describe this file") without effect.
                    LOG_INFO("Discarding unreadable shard for {}",
                             workspace.path_pool.resolve(path_id));
                    enqueue(path_id, ReindexReason::ContentChanged);
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

bool Indexer::need_update(llvm::StringRef file_path) {
    auto merged_it = workspace.merged_indices.find(workspace.path_pool.intern(file_path));
    if(merged_it == workspace.merged_indices.end())
        return true;

    return merged_it->second.need_update();
}

void Indexer::enqueue(std::uint32_t server_path_id, ReindexReason reason) {
    // A fresh slot means any prior slot was already consumed (or none
    // existed); a queued-and-unconsumed slot makes this call a duplicate.
    bool fresh_slot = pending_ids.insert(server_path_id).second;

    // Record (or refresh) why the file is pending. Within one queued slot
    // ContentChanged is absorbing: a deps-only cascade cannot downgrade a
    // file whose own content already changed. Across slots it is not: a
    // deps-only requeue after the previous slot was consumed is new debt of
    // its own kind — the in-flight (or finished) pass already covers the
    // earlier content change, and keeping ContentChanged would suppress the
    // file's rows past that pass. The fresh ticket invalidates the clear of
    // any index task already in flight for this file.
    ++reindex_ticket;
    auto [it, inserted] =
        reindex_reasons.try_emplace(server_path_id,
                                    reason,
                                    reindex_ticket,
                                    reason == ReindexReason::ContentChanged ? reindex_ticket : 0);
    if(!inserted) {
        if(reason == ReindexReason::ContentChanged) {
            it->second.reason = ReindexReason::ContentChanged;
            it->second.content_ticket = reindex_ticket;
            // New content starts a fresh poison budget: the crashes the
            // old bytes caused say nothing about the fixed ones, and a
            // stale ledger would abandon the file on its first hiccup.
            it->second.requeue_attempts = 0;
        } else if(fresh_slot) {
            it->second.reason = ReindexReason::DepsOnly;
        }
        it->second.ticket = reindex_ticket;
    }

    if(!fresh_slot)
        return;
    index_queue.push_back(server_path_id);
}

void Indexer::pause_indexing() {
    ++pause_depth;
    if(pause_depth == 1) {
        resume_event.reset();
        LOG_DEBUG("Background indexing paused");
    }
}

void Indexer::resume_indexing() {
    if(pause_depth > 0)
        --pause_depth;
    if(pause_depth == 0) {
        resume_event.set();
        LOG_DEBUG("Background indexing resumed");
    }
}

kota::task<> Indexer::stop() {
    bg_tasks.cancel();
    co_await bg_tasks.join();
}

void Indexer::schedule() {
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

kota::task<> Indexer::index_one(std::uint32_t server_path_id,
                                std::uint64_t ticket,
                                std::size_t index,
                                std::size_t total) {
    auto file_path = std::string(workspace.path_pool.resolve(server_path_id));

    // Open files are skipped until an agent shows up: the LSP side never
    // reads their shards (sessions serve them), so indexing them is pure
    // waste — but agents read disk truth and need the shards, snapshot
    // taken from disk regardless of the live buffer. Skipping loses no
    // debt: BufferClosed re-checks the shard against the disk on close.
    if(!index_open_files && sessions.find(server_path_id) != nullptr)
        co_return;

    // The engine's own observation is authoritative for content changes:
    // it saw the event. The dep-hash check below cannot be trusted to see
    // a file's own edit (it validates the recorded dependencies), so only
    // deps-only slots — where it exists to deduplicate cascade storms —
    // may take the shortcut.
    if(auto it = reindex_reasons.find(server_path_id);
       (it == reindex_reasons.end() || it->second.reason != ReindexReason::ContentChanged) &&
       !need_update(file_path)) {
        co_return;
    }

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
        // Merge guard: a newer content-level invalidation during this build
        // (or a removal clearing the entry) means this result describes text
        // that no longer exists — e.g. a compile-command change whose
        // erase+re-enqueue must not be undone by an in-flight merge of the
        // old-command rows. Drop the merge; the follow-up slot redoes it.
        // A deps-only requeue is deliberately NOT superseding: the in-flight
        // rows are positionally right, and suppressing them would trade a
        // tolerated semantic drift for a coverage hole.
        if(auto it = reindex_reasons.find(server_path_id);
           it == reindex_reasons.end() || it->second.content_ticket > ticket) {
            LOG_INFO("Discarding superseded index result for {}", file_path);
            co_return;
        }
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
    } else if(result.error().code == worker::dispatch_errc::cancelled ||
              result.error().code == worker::dispatch_errc::worker_crashed ||
              (result.error().code == worker::dispatch_errc::worker_unavailable &&
               pool.revives_slots())) {
        // Preempted under memory pressure or lost to a worker crash: the
        // work itself is fine — requeue the file with its original reason so
        // the next round redoes it instead of silently dropping coverage.
        // worker_unavailable requeues (budget-free, like a preemption) only
        // when the pool revives dead slots: the outage is then a window, not
        // a verdict, and each retry round waits out the idle timer rather
        // than spinning. Without revival a requeue could never succeed.
        bool crashed = result.error().code == worker::dispatch_errc::worker_crashed;
        switch(note_dispatch_failure(server_path_id, ticket, crashed)) {
            case RequeueVerdict::Dropped: {
                LOG_INFO("[{}/{}] Index dropped for removed file {}", index, total, file_path);
                break;
            }
            case RequeueVerdict::Superseded: {
                LOG_INFO("[{}/{}] Index failure for superseded content of {}",
                         index,
                         total,
                         file_path);
                break;
            }
            case RequeueVerdict::GaveUp: {
                // Log-only by design: the file is usually not open (open
                // documents are served by their session, not the shard),
                // so there is no diagnostic surface. Cross-file references
                // into this file stay stale until its content changes.
                LOG_WARN(
                    "[{}/{}] Index giving up on {} after {} crash requeues; " "its cross-file data stays stale until it is edited: {}",
                    index,
                    total,
                    file_path,
                    max_requeue_attempts,
                    result.error().message);
                break;
            }
            case RequeueVerdict::Requeued: {
                LOG_INFO("[{}/{}] Index requeued for {}: {}",
                         index,
                         total,
                         file_path,
                         result.error().message);
                break;
            }
        }
    } else {
        LOG_WARN("[{}/{}] Index IPC error for {}: {}",
                 index,
                 total,
                 file_path,
                 result.error().message);
    }
}

auto Indexer::note_dispatch_failure(std::uint32_t server_path_id,
                                    std::uint64_t ticket,
                                    bool crashed) -> RequeueVerdict {
    // Only while the pending entry survives: a file removed from disk
    // mid-flight was cleared and has nothing to redo.
    auto it = reindex_reasons.find(server_path_id);
    if(it == reindex_reasons.end()) {
        return RequeueVerdict::Dropped;
    }

    // The failed dispatch carried bytes a ContentChanged enqueue has since
    // replaced: its crash says nothing about the fixed content, so it
    // neither spends the fresh budget nor requeues — the newer content's
    // own slot redoes the work.
    if(it->second.content_ticket > ticket) {
        return RequeueVerdict::Superseded;
    }

    // The budget both counts and gates crashes only: a preemption under
    // memory pressure says nothing about the file, so it requeues even
    // when the crash budget is already spent — giving up on it would
    // clear the pending state and serve the stale shard as fresh.
    if(crashed) {
        if(it->second.requeue_attempts >= max_requeue_attempts) {
            // Giving up accepts the staleness, so clear the pending slot
            // here rather than relying on run_index_task's ticket-guarded
            // clear: a deps-only enqueue that landed mid-flight bumped the
            // ticket, and the guard would leave that downgraded entry
            // queued — a doomed retry that spends one more worker.
            clear_pending(server_path_id);
            return RequeueVerdict::GaveUp;
        }
        it->second.requeue_attempts += 1;
    }
    // Requeue with the debt class this dispatch carried, not the entry's
    // current one: a deps-only enqueue that landed mid-flight downgraded
    // the reason betting on this content pass to land — a failed pass
    // leaves the edit uncovered, and only a ContentChanged pending entry
    // keeps suppressing the stale shard.
    auto reason =
        it->second.content_ticket == ticket ? ReindexReason::ContentChanged : it->second.reason;
    // The enqueue bumps the entry's ticket, which shields it from the
    // in-flight task's pending-state clear. It also resets the poison
    // budget on ContentChanged — right for a user edit, wrong for this
    // requeue of the same bytes — so restore the ledger afterwards
    // (try_emplace on the existing key keeps `it` valid).
    auto attempts = it->second.requeue_attempts;
    enqueue(server_path_id, reason);
    it->second.requeue_attempts = attempts;
    return RequeueVerdict::Requeued;
}

kota::task<> Indexer::run_index_task(std::uint32_t server_path_id,
                                     std::uint64_t ticket,
                                     std::size_t index,
                                     std::size_t total,
                                     std::size_t& completed) {
    co_await index_one(server_path_id, ticket, index, total);
    // The pending window ends with the index attempt, success or not. On
    // failure the last-known rows resume serving — deliberately: keeping
    // the gate would hide a file that fails to index (broken compile,
    // missing command) from every cross-file query with no recovery path,
    // since only a future event re-enqueues it. Any such event re-judges
    // staleness by content hash. A re-enqueue during the flight bumped
    // the ticket: that newer pending state must survive this clear.
    if(auto it = reindex_reasons.find(server_path_id);
       it != reindex_reasons.end() && it->second.ticket == ticket) {
        reindex_reasons.erase(it);
    }
    ++completed;
    progress_data.stage = Progress::Stage::Report;
    progress_data.completed = completed;
    on_progress_changed.emit();
}

kota::task<> Indexer::run_background_indexing() {
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
        // No open-session or hash-freshness shortcut here: index_one is the
        // single decision point for skipping (it knows the pending reason;
        // a hash check alone cannot see a file's own edit), and the
        // completion clear in run_index_task retires the pending state with
        // the ticket honored. A second, reason-blind copy of these checks
        // here is exactly what once erased ContentChanged state early and
        // let a stale shard keep serving.

        // A queued slot with no pending entry was cleared mid-batch: the
        // file was removed from disk after being enqueued (clear_pending),
        // so there is nothing to index — skip the slot. Every other slot
        // has an entry, because enqueue writes it before the queue push.
        auto pending_it = reindex_reasons.find(server_path_id);
        if(pending_it == reindex_reasons.end()) {
            continue;
        }

        ++dispatched;
        auto ticket = pending_it->second.ticket;
        // A member coroutine, not an immediately-invoked capturing lambda:
        // a lambda's captures live in the lambda object, which dies at the
        // end of this statement — anything read after the first suspension
        // would dangle. Coroutine parameters are copied into the frame.
        workers.spawn(run_index_task(server_path_id, ticket, dispatched, total, completed));
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
        assert(reindex_reasons.empty() && "drained queue must have no pending reasons");
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

    // Files enqueued while the round was joining its workers saw their
    // schedule() no-op against indexing_active; without this kick they
    // would wait for the next external event — and a content-changed
    // pending file's rows stay skipped for that whole wait.
    if(index_queue_pos < index_queue.size()) {
        schedule();
    }
}

}  // namespace clice

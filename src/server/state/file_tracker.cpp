#include "server/state/file_tracker.h"

#include <algorithm>
#include <chrono>
#include <utility>

#include "support/logging.h"
#include "support/timer.h"

#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/ScopeExit.h"
#include "llvm/Support/Chrono.h"
#include "llvm/Support/FileSystem.h"

namespace clice {

static std::int64_t to_nanoseconds(const llvm::sys::TimePoint<>& time) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(time.time_since_epoch()).count();
}

FileTracker::FileTracker(Workspace& workspace,
                         const SessionStore& store,
                         std::string workspace_root) :
    workspace(workspace), store(store), workspace_root(std::move(workspace_root)) {
    cdb_path = discover_compile_commands(workspace.config, this->workspace_root);
    // A change landing between the workspace load and this stat is caught
    // anyway: the stamp only gates reloads, and the reload's diff is
    // computed from content, so it never reports spurious changes.
    applied = stat_cdb();
}

FileTracker::CDBStamp FileTracker::stat_cdb() const {
    CDBStamp stamp;
    if(cdb_path.empty()) {
        return stamp;
    }
    llvm::sys::fs::file_status status;
    if(llvm::sys::fs::status(cdb_path, status)) {
        return stamp;
    }
    stamp.exists = true;
    stamp.size = status.getSize();
    stamp.mtime_ns = to_nanoseconds(status.getLastModificationTime());
    return stamp;
}

llvm::SmallVector<FileEvent> FileTracker::tick_cdb(bool force) {
    if(cdb_path.empty()) {
        cdb_path = discover_compile_commands(workspace.config, workspace_root);
        if(cdb_path.empty()) {
            return {};
        }
        // `applied` stays at its missing state: the fresh file is a change
        // against the never-loaded database and goes through the normal
        // settle-and-reload path below.
        LOG_INFO("Found compilation database: {}", cdb_path);
    }

    auto current = stat_cdb();
    if(!force) {
        if(current == applied) {
            has_pending = false;
            return {};
        }
        // Generators rewrite the file in place; only act once the stamp
        // has been stable for two consecutive ticks (half-write guard).
        if(!has_pending || !(pending == current)) {
            pending = current;
            has_pending = true;
            return {};
        }
    }
    // A forced tick reloads unconditionally — the stamp gate would make a
    // same-size rewrite within mtime granularity invisible to the test
    // hook, and a spurious reload just yields an empty diff.
    has_pending = false;

    if(!current.exists) {
        // Deleted — usually mid-regeneration. Keep serving the loaded
        // entries; the rewrite lands as the next observed change. Forget
        // the path too: if the database reappears somewhere else among the
        // configured locations, discovery must run again.
        applied = current;
        cdb_path.clear();
        return {};
    }

    auto diff = workspace.cdb.reload_and_diff(cdb_path);
    if(!diff) {
        // Stats fine but unreadable right now (e.g. still locked by the
        // generator). Leave `applied` alone: the stamp stays different, so
        // the reload is retried on a later tick instead of being lost.
        return {};
    }
    applied = current;
    LOG_INFO("Reloaded CDB from {}: {} added, {} removed, {} changed",
             cdb_path,
             diff->added.size(),
             diff->removed.size(),
             diff->changed.size());
    if(diff->empty()) {
        return {};
    }

    // The diff speaks in the CDB's own path ids; events speak in master
    // path-pool ids.
    FileEvent::CDBDelta delta;
    auto convert = [&](llvm::ArrayRef<std::uint32_t> cdb_ids,
                       llvm::SmallVectorImpl<std::uint32_t>& out) {
        for(auto id: cdb_ids) {
            out.push_back(workspace.path_pool.intern(workspace.cdb.resolve_path(id)));
        }
    };
    convert(diff->added, delta.added);
    convert(diff->removed, delta.removed);
    convert(diff->changed, delta.changed);

    llvm::SmallVector<FileEvent> events;
    events.push_back(FileEvent::cdb_changed(std::move(delta)));
    return events;
}

kota::task<llvm::SmallVector<FileEvent>> FileTracker::tick_workspace() {
    constexpr std::size_t batch_size = 500;

    if(sweeping) {
        // The poll hook can land while the live loop is suspended between
        // batches; two interleaved sweeps would race on the baseline. The
        // running sweep already covers this request.
        co_return llvm::SmallVector<FileEvent>{};
    }
    sweeping = true;
    auto guard = llvm::make_scope_exit([this] { sweeping = false; });

    ScopedTimer timer;
    auto epoch = workspace.context_epoch;
    auto files = workspace.dep_graph.all_files();

    // Files that left the graph (e.g. a CDB reload rebuilt it) stop being
    // tracked; their baseline entries would otherwise be stat'd forever.
    llvm::DenseSet<std::uint32_t> known(files.begin(), files.end());
    llvm::SmallVector<std::uint32_t> gone;
    for(auto& [path_id, state]: baseline) {
        if(!known.contains(path_id)) {
            gone.push_back(path_id);
        }
    }
    for(auto path_id: gone) {
        baseline.erase(path_id);
    }

    llvm::SmallVector<FileEvent> events;
    std::size_t changed = 0;
    std::size_t removed = 0;
    for(std::size_t begin = 0; begin < files.size(); begin += batch_size) {
        if(begin != 0) {
            // Yield one loop iteration between batches so a long sweep
            // never starves LSP traffic.
            co_await kota::sleep(std::chrono::milliseconds(0));
        }

        auto batch_end = std::min(begin + batch_size, files.size());
        for(std::size_t i = begin; i < batch_end; ++i) {
            auto path_id = files[i];
            if(store.find(path_id)) {
                // Open buffers are the truth and didSave owns their disk
                // sync; drop the baseline so the file re-seeds silently
                // once it closes (BufferClosed already reindexes it).
                // TODO: a disk change landing while the file is open (git
                // checkout on an open header, closed without saving) is
                // forgotten by this reset — dependents are not cascaded.
                // Desync hardening owns that case.
                baseline.erase(path_id);
                continue;
            }

            auto path = workspace.path_pool.resolve(path_id);
            llvm::sys::fs::file_status status;
            bool exists = !llvm::sys::fs::status(path, status);

            auto it = baseline.find(path_id);
            if(it == baseline.end()) {
                // First sight seeds the baseline silently.
                FileState state;
                state.missing = !exists;
                if(exists) {
                    state.size = status.getSize();
                    state.mtime_ns = to_nanoseconds(status.getLastModificationTime());
                    state.hash = hash_file(path);
                    if(state.hash == 0) {
                        // Read failure (hash_file's sentinel): don't seed a
                        // baseline that would later compare as a change.
                        // Retry next tick.
                        continue;
                    }
                }
                baseline.try_emplace(path_id, state);
                continue;
            }

            auto& state = it->second;
            if(!exists) {
                if(!state.missing) {
                    state = FileState{.missing = true};
                    events.push_back(FileEvent::disk_removed(path_id));
                    removed += 1;
                }
                continue;
            }

            auto size = status.getSize();
            auto mtime_ns = to_nanoseconds(status.getLastModificationTime());
            if(!state.missing && state.size == size && state.mtime_ns == mtime_ns) {
                continue;
            }

            // The stamp moved: only a confirmed content change counts, so
            // touches and checkouts of identical bytes stay silent.
            auto hash = hash_file(path);
            if(hash == 0) {
                // The file stats fine but cannot be read right now (e.g. an
                // antivirus scanner briefly holding a fresh file on Windows).
                // No signal either way — leave the baseline untouched so the
                // still-different stamp retries this check next tick, and
                // emit nothing: a failed read must never count as a change.
                continue;
            }
            bool content_changed = state.missing || hash != state.hash;
            state = FileState{.size = size, .mtime_ns = mtime_ns, .hash = hash};
            if(content_changed) {
                events.push_back(FileEvent::disk_changed(path_id));
                changed += 1;
            }
        }
    }

    // A CDB reload can rebuild the include graph while this sweep is
    // suspended between batches. Events for files the new graph no longer
    // tracks must not dispatch — their rescan cascade would reintroduce
    // edges for files that stopped being sources. Dropping them is safe:
    // their baseline entries are pruned on the next sweep.
    if(workspace.context_epoch != epoch && !events.empty()) {
        auto current = workspace.dep_graph.all_files();
        llvm::DenseSet<std::uint32_t> still_known(current.begin(), current.end());
        llvm::erase_if(events, [&](const FileEvent& event) {
            return !still_known.contains(event.path_id);
        });
    }

    LOG_PERF("tracker",
             "phase=workspace_sweep files={} changed={} removed={} elapsed_ms={}",
             files.size(),
             changed,
             removed,
             timer.ms());
    co_return events;
}

}  // namespace clice

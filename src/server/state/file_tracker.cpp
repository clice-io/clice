#include "server/state/file_tracker.h"

#include <algorithm>
#include <chrono>
#include <optional>
#include <ranges>
#include <utility>

#include "support/filesystem.h"
#include "support/logging.h"
#include "support/timer.h"

#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/Support/Chrono.h"
#include "llvm/Support/FileSystem.h"

namespace clice {

FileTracker::FileTracker(Project& project, const SessionStore& store, std::string root) :
    project(project), store(store), cdb(project, std::move(root)) {}

/// Diff ids and event ids share the single file table.
static void push_delta(const CDBDiff& diff, llvm::SmallVectorImpl<FileEvent>& events) {
    if(diff.empty()) {
        return;
    }
    FileEvent::CDBDelta delta;
    delta.added.assign(diff.added.begin(), diff.added.end());
    delta.removed.assign(diff.removed.begin(), diff.removed.end());
    delta.changed.assign(diff.changed.begin(), diff.changed.end());
    events.push_back(FileEvent::cdb_changed(std::move(delta)));
}

llvm::SmallVector<FileEvent> FileTracker::tick_cdb(bool force) {
    llvm::SmallVector<Fid> open_files;
    for(auto& [path_id, session]: store.sessions) {
        open_files.push_back(path_id);
    }
    llvm::SmallVector<FileEvent> events;
    push_delta(cdb.tick(open_files, force), events);
    return events;
}

llvm::SmallVector<FileEvent> FileTracker::discover_around(Fid path_id) {
    llvm::SmallVector<FileEvent> events;
    push_delta(cdb.discover_around(path_id), events);
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
    auto epoch = project.context_epoch;
    auto files = project.dep_graph.all_files();

    // Files that left the graph (e.g. a CDB reload rebuilt it) stop being
    // tracked; their baseline entries would otherwise be stat'd forever.
    llvm::DenseSet<Fid> known(files.begin(), files.end());
    llvm::SmallVector<Fid> gone;
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
            auto scanned = project.dep_graph.scanned_hash(path_id);
            if(store.find(path_id)) {
                // Open buffers are the truth: didSave and didClose own their
                // disk sync. The baseline follows what the project derived
                // from the file (a save rescans it), with its stat forgotten
                // so the first sweep after the close compares content, and
                // keeps its presence — a file deleted while open is left to
                // that sweep by BufferClosed.
                if(scanned) {
                    baseline[path_id] = FileState{.hash = *scanned};
                }
                continue;
            }

            auto path = project.file_table.resolve(path_id);
            llvm::sys::fs::file_status status;
            bool exists = !llvm::sys::fs::status(path, status);

            auto it = baseline.find(path_id);
            if(it == baseline.end() && scanned) {
                // First sight of a file a scan read: what the project derived
                // from it is the baseline, so a change landing between that
                // scan and this sweep still counts.
                it = baseline.try_emplace(path_id, FileState{.hash = *scanned}).first;
            } else if(it == baseline.end()) {
                // First sight of a file nothing derived from yet seeds the
                // baseline silently.
                FileState state;
                state.missing = !exists;
                if(exists) {
                    auto obs = project.file_table.observe_for(path_id, status);
                    if(!obs) {
                        // Unreadable right now: don't seed a baseline that
                        // would later compare as a change. Retry next tick.
                        continue;
                    }
                    state.size = obs->size;
                    state.mtime_ns = obs->mtime_ns;
                    state.hash = obs->hash;
                    state.uid_device = obs->uid_device;
                    state.uid_file = obs->uid_file;
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
            auto mtime_ns = fs::mtime_ns(status);
            auto uid = status.getUniqueID();
            if(!state.missing && state.size == size && state.mtime_ns == mtime_ns &&
               (!fs::stable_file_ids ||
                (state.uid_device == uid.getDevice() && state.uid_file == uid.getFile()))) {
                continue;
            }

            // The stamp moved: only a confirmed content change counts, so
            // touches and checkouts of identical bytes stay silent.
            auto obs = project.file_table.observe_for(path_id, status);
            if(!obs) {
                // The file stats fine but cannot be read right now (e.g. an
                // antivirus scanner briefly holding a fresh file on Windows).
                // No signal either way — leave the baseline untouched so the
                // still-different stamp retries this check next tick, and
                // emit nothing: a failed read must never count as a change.
                continue;
            }
            bool content_changed = state.missing || obs->hash != state.hash;
            state = FileState{.size = obs->size,
                              .mtime_ns = obs->mtime_ns,
                              .hash = obs->hash,
                              .uid_device = obs->uid_device,
                              .uid_file = obs->uid_file};
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
    if(project.context_epoch != epoch && !events.empty()) {
        auto current = project.dep_graph.all_files();
        llvm::DenseSet<Fid> still_known(current.begin(), current.end());
        llvm::erase_if(events, [&](const FileEvent& event) {
            return !still_known.contains(event.path_id);
        });
    }

    // A file created under a default-command rule joins the build: the
    // same gain of a command a database reload reports as added. One
    // deleted left through DiskRemoved above, like any tracked file.
    push_delta({.added = project.build.refresh_default_sources()}, events);

    LOG_PERF("tracker",
             "phase=workspace_sweep files={} changed={} removed={} elapsed_ms={}",
             files.size(),
             changed,
             removed,
             timer.ms());
    co_return events;
}

}  // namespace clice

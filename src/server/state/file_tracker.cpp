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

FileTracker::FileTracker(Workspace& workspace,
                         const SessionStore& store,
                         std::string workspace_root) :
    workspace(workspace), store(store), workspace_root(std::move(workspace_root)) {
    // Discovery compares the root with the file table's spellings.
    path::canonicalize(this->workspace_root);
    // A change landing between the workspace load and this stat is caught
    // anyway: the stamp only gates reloads, and the reload's diff is
    // computed from content, so it never reports spurious changes.
    for(std::size_t i = 0; i < workspace.cdb.source_count(); i += 1) {
        track(SourceID(i));
    }
}

FileTracker::FileStamp FileTracker::stat_file(llvm::StringRef path) {
    FileStamp stamp;
    llvm::sys::fs::file_status status;
    if(path.empty() || llvm::sys::fs::status(path, status)) {
        return stamp;
    }
    stamp.exists = true;
    stamp.size = status.getSize();
    stamp.mtime_ns = fs::mtime_ns(status);
    if(fs::stable_file_ids) {
        auto uid = status.getUniqueID();
        stamp.uid_device = uid.getDevice();
        stamp.uid_file = uid.getFile();
    }
    return stamp;
}

FileTracker::SourceStamp FileTracker::stat_source(SourceID id) const {
    SourceStamp stamp{.database = stat_file(workspace.cdb.source_path(id))};
    for(auto& response: workspace.cdb.response_files(id).take_front(watched_responses)) {
        stamp.responses.push_back(stat_file(response));
    }
    return stamp;
}

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

void FileTracker::track(SourceID id) {
    // A source whose startup load failed (unreadable, mid-rewrite) stays
    // baselined as missing, so the next tick reloads it even when its
    // stamp never changes.
    TrackedSource tracked{.id = id};
    if(workspace.cdb.loaded(id)) {
        tracked.applied = stat_source(id);
    }
    sources.push_back(std::move(tracked));
}

llvm::SmallVector<Fid> FileTracker::shared_files(SourceID id) const {
    llvm::SmallVector<Fid> shared;
    for(auto group: workspace.cdb.entries() | std::views::chunk_by([](const CompilationEntry& a,
                                                                      const CompilationEntry& b) {
                        return a.file == b.file;
                    })) {
        auto listed = [&](const CompilationEntry& entry) {
            return entry.source == id;
        };
        if(std::ranges::any_of(group, listed) && !std::ranges::all_of(group, listed)) {
            shared.push_back(group.front().file);
        }
    }
    return shared;
}

llvm::SmallVector<SourceID> FileTracker::default_sources(llvm::ArrayRef<Fid> files) const {
    return llvm::to_vector(llvm::map_range(files, [&](Fid file) {
        return workspace.build.entries(file).front().source;
    }));
}

/// The files whose default entry moved between two rankings, into
/// `changed`.
static void push_moved(llvm::ArrayRef<Fid> files,
                       llvm::ArrayRef<SourceID> before,
                       llvm::ArrayRef<SourceID> after,
                       llvm::SmallVectorImpl<Fid>& changed) {
    for(auto [file, was, now]: llvm::zip(files, before, after)) {
        if(was != now && !llvm::is_contained(changed, file)) {
            changed.push_back(file);
        }
    }
}

void FileTracker::tick_source(TrackedSource& tracked,
                              bool force,
                              llvm::SmallVectorImpl<FileEvent>& events) {
    auto current = stat_source(tracked.id);
    if(!force) {
        if(current == tracked.applied) {
            tracked.has_pending = false;
            return;
        }
        // Generators rewrite the file in place; only act once the stamp
        // has been stable for two consecutive ticks (half-write guard).
        if(!tracked.has_pending || !(tracked.pending == current)) {
            tracked.pending = current;
            tracked.has_pending = true;
            return;
        }
    }
    // A forced tick reloads unconditionally — the stamp gate would make a
    // same-size rewrite within mtime granularity invisible to the test
    // hook, and a spurious reload just yields an empty diff.
    tracked.has_pending = false;
    // A discovered database's presence ranks it (see Build::source_order):
    // the files whose default entry moves with it change command.
    bool flips = tracked.applied.database.exists != current.database.exists &&
                 workspace.build.discovered(tracked.id);
    llvm::SmallVector<Fid> shared;
    llvm::SmallVector<SourceID> before;
    if(flips) {
        shared = shared_files(tracked.id);
        before = default_sources(shared);
    }
    if(!current.database.exists) {
        // Deleted — usually mid-regeneration. Keep serving the loaded
        // entries; the rewrite lands as the next observed change.
        tracked.applied = current;
        workspace.cdb.set_present(tracked.id, false);
        if(flips) {
            CDBDiff moved;
            push_moved(shared, before, default_sources(shared), moved.changed);
            push_delta(moved, events);
        }
        return;
    }
    llvm::StringMap<FileStamp> known;
    for(auto [response, stamp]:
        llvm::zip(workspace.cdb.response_files(tracked.id), current.responses)) {
        known[response] = stamp;
    }
    auto diff = workspace.cdb.reload_and_diff(tracked.id);
    if(!diff) {
        // Stats fine but unreadable right now (e.g. still locked by the
        // generator). Leave `applied` alone: the stamp stays different, so
        // the reload is retried on a later tick instead of being lost.
        return;
    }
    // The stamps predate the read, so a rewrite landing meanwhile is seen
    // next tick. A response file this reload first named has no such
    // stamp: baselined as unknown, it reloads once more after settling,
    // with one taken before that read.
    tracked.applied = current;
    tracked.applied.responses.clear();
    for(auto& response: workspace.cdb.response_files(tracked.id).take_front(watched_responses)) {
        auto it = known.find(response);
        tracked.applied.responses.push_back(it != known.end() ? it->second : FileStamp{});
    }
    LOG_INFO("Reloaded CDB from {}: {} added, {} removed, {} changed",
             workspace.cdb.source_path(tracked.id),
             diff->added.size(),
             diff->removed.size(),
             diff->changed.size());
    if(flips) {
        push_moved(shared, before, default_sources(shared), diff->changed);
    }
    push_delta(*diff, events);
}

llvm::SmallVector<FileEvent> FileTracker::tick_cdb(bool force) {
    llvm::SmallVector<FileEvent> events;
    // Nothing declared: keep looking, so a database generated after
    // startup — at the root, in a new subdirectory, or above a file open
    // without one — is picked up. Declared sources are registered
    // (existing or not) and only watched.
    if(!workspace.build.declares_sources()) {
        for(auto& found: discover_compile_commands(workspace_root)) {
            auto id = workspace.cdb.add_source(found);
            if(llvm::none_of(sources,
                             [&](const TrackedSource& tracked) { return tracked.id == id; })) {
                // Baselined as missing: the fresh file is a change against
                // the never-loaded source and goes through the normal
                // settle-and-reload path.
                LOG_INFO("Found compilation database: {}", found);
                track(id);
            }
        }
        for(auto& [path_id, session]: store.sessions) {
            events.append(discover_around(path_id));
        }
    }
    for(auto& tracked: sources) {
        tick_source(tracked, force, events);
    }
    return events;
}

llvm::SmallVector<FileEvent> FileTracker::discover_around(Fid path_id) {
    llvm::SmallVector<FileEvent> events;
    if(workspace.build.declares_sources() || !workspace.build.commands(path_id).empty()) {
        return events;
    }
    auto path = workspace.file_table.resolve(path_id);
    if(!path::under(path, workspace_root)) {
        return events;
    }
    for(auto& database: compile_commands_above(path::parent_path(path), workspace_root)) {
        if(workspace.cdb.find_source(database)) {
            continue;
        }
        auto id = workspace.cdb.add_source(database);
        if(auto diff = workspace.cdb.reload_and_diff(id)) {
            LOG_INFO("Found compilation database: {}", database);
            push_delta(*diff, events);
        }
        track(id);
    }
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

            auto path = workspace.file_table.resolve(path_id);
            llvm::sys::fs::file_status status;
            bool exists = !llvm::sys::fs::status(path, status);

            auto it = baseline.find(path_id);
            if(it == baseline.end()) {
                // First sight seeds the baseline silently. The startup
                // scan usually observed the file already, so the common
                // seed is a shared-pair hit with no second read.
                FileState state;
                state.missing = !exists;
                if(exists) {
                    auto obs = workspace.file_table.observe_for(path_id, status);
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
            auto obs = workspace.file_table.observe_for(path_id, status);
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
    if(workspace.context_epoch != epoch && !events.empty()) {
        auto current = workspace.dep_graph.all_files();
        llvm::DenseSet<Fid> still_known(current.begin(), current.end());
        llvm::erase_if(events, [&](const FileEvent& event) {
            return !still_known.contains(event.path_id);
        });
    }

    // A file created under a default-command rule joins the build: the
    // same gain of a command a database reload reports as added. One
    // deleted leaves through DiskRemoved, but no longer lends.
    auto refresh = workspace.build.refresh_default_sources();
    if(refresh.vanished || !refresh.appeared.empty()) {
        workspace.commands_epoch += 1;
    }
    if(!refresh.appeared.empty()) {
        push_delta({.added = std::move(refresh.appeared)}, events);
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

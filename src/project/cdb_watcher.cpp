#include "project/cdb_watcher.h"

#include <ranges>
#include <utility>

#include "support/filesystem.h"
#include "support/logging.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/Support/FileSystem.h"

namespace clice {

CDBWatcher::CDBWatcher(Project& project, std::string root) :
    project(project), root(std::move(root)) {
    // Discovery compares the root with the file table's spellings.
    path::canonicalize(this->root);
    for(std::size_t i = 0; i < project.cdb.source_count(); i += 1) {
        track(SourceID(i));
    }
}

CDBWatcher::FileStamp CDBWatcher::stat_file(llvm::StringRef path) {
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

CDBWatcher::FileStamp CDBWatcher::stamp_of(const DiskObservation& observed) {
    FileStamp stamp{.exists = true, .size = observed.size, .mtime_ns = observed.mtime_ns};
    if(fs::stable_file_ids) {
        stamp.uid_device = observed.uid_device;
        stamp.uid_file = observed.uid_file;
    }
    return stamp;
}

CDBWatcher::SourceStamp CDBWatcher::stat_source(SourceID id) const {
    SourceStamp stamp{.database = stat_file(project.cdb.source_path(id))};
    for(auto& response: project.cdb.response_files(id)) {
        stamp.responses.push_back(stat_file(response));
    }
    return stamp;
}

void CDBWatcher::track(SourceID id) {
    // A source whose startup load failed (unreadable, mid-rewrite) stays
    // baselined as missing, so the next tick reloads it even when its
    // stamp never changes.
    TrackedSource tracked{.id = id};
    if(project.cdb.loaded(id)) {
        tracked.applied = stat_source(id);
        tracked.reread = !project.cdb.response_files(id).empty();
        // Loaded, then deleted before this baseline: the load marked it
        // present, and an unchanged missing stamp would never correct it.
        project.cdb.set_present(id, tracked.applied.database.exists);
        if(tracked.applied.database.exists) {
            adopt_load(tracked);
        }
    }
    sources.push_back(std::move(tracked));
}

void CDBWatcher::adopt_load(TrackedSource& tracked) {
    auto& observed = project.cdb.observation(tracked.id);
    tracked.applied.database = stamp_of(observed);
    tracked.hash = observed.hash;
    tracked.trusted = observed.reliable;
}

bool CDBWatcher::content_unchanged(TrackedSource& tracked) {
    auto observed = read_file_observed(std::string(project.cdb.source_path(tracked.id)).c_str());
    if(!observed) {
        // Unreadable right now: no evidence either way, and the baseline
        // stays untrusted, so the next tick looks again.
        return true;
    }
    if(observed->obs.hash != tracked.hash) {
        return false;
    }
    tracked.applied.database = stamp_of(observed->obs);
    tracked.trusted = observed->obs.reliable;
    return true;
}

llvm::SmallVector<Fid> CDBWatcher::shared_files(SourceID id) const {
    llvm::SmallVector<Fid> shared;
    for(auto group: project.cdb.entries() | std::views::chunk_by([](const CompilationEntry& a,
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

llvm::SmallVector<std::optional<SourceID>>
    CDBWatcher::default_sources(llvm::ArrayRef<Fid> files) const {
    return llvm::to_vector(llvm::map_range(files, [&](Fid file) -> std::optional<SourceID> {
        auto entries = project.build.entries(file);
        if(entries.empty()) {
            return std::nullopt;
        }
        return entries.front().source;
    }));
}

/// The files whose default entry moved between two rankings, into
/// `changed`.
static void push_moved(llvm::ArrayRef<Fid> files,
                       llvm::ArrayRef<std::optional<SourceID>> before,
                       llvm::ArrayRef<std::optional<SourceID>> after,
                       llvm::SmallVectorImpl<Fid>& changed) {
    for(auto [file, was, now]: llvm::zip(files, before, after)) {
        if(was != now && !llvm::is_contained(changed, file)) {
            changed.push_back(file);
        }
    }
}

/// Deltas of one tick merge: the invalidator rebuilds the graph per event.
static void append(CDBDiff& into, const CDBDiff& from) {
    into.added.append(from.added);
    into.removed.append(from.removed);
    into.changed.append(from.changed);
}

void CDBWatcher::tick_source(TrackedSource& tracked, bool force, CDBDiff& delta) {
    auto current = stat_source(tracked.id);
    if(!force) {
        if(current == tracked.applied && !tracked.reread) {
            tracked.has_pending = false;
            if(tracked.trusted || content_unchanged(tracked)) {
                return;
            }
            // Other bytes under the loaded stamp: a rewrite within the mtime
            // granularity of the load. No stamp change will ever announce
            // it, so there is nothing to settle — reload now.
        } else if(!tracked.has_pending || !(tracked.pending == current)) {
            // Generators rewrite the file in place; only act once the stamp
            // has been stable for two consecutive ticks (half-write guard).
            tracked.pending = current;
            tracked.has_pending = true;
            return;
        }
    }
    // A forced tick reloads unconditionally: a spurious reload just yields
    // an empty diff.
    tracked.has_pending = false;
    // A discovered database's presence ranks it (see Build::source_order):
    // the files whose default entry moves with it change command.
    bool flips = tracked.applied.database.exists != current.database.exists &&
                 project.build.discovered(tracked.id);
    llvm::SmallVector<Fid> shared;
    llvm::SmallVector<std::optional<SourceID>> before;
    if(flips) {
        shared = shared_files(tracked.id);
        before = default_sources(shared);
    }
    if(!current.database.exists) {
        // Deleted — usually mid-regeneration. Keep serving the loaded
        // entries; the rewrite lands as the next observed change.
        tracked.applied = current;
        tracked.trusted = true;
        tracked.reread = false;
        project.cdb.set_present(tracked.id, false);
        if(flips) {
            push_moved(shared, before, default_sources(shared), delta.changed);
        }
        return;
    }
    llvm::StringMap<FileStamp> known;
    for(auto [response, stamp]:
        llvm::zip(project.cdb.response_files(tracked.id), current.responses)) {
        known[response] = stamp;
    }
    auto diff = project.cdb.reload_and_diff(tracked.id);
    if(!diff) {
        // Stats fine but unreadable right now (e.g. still locked by the
        // generator). Leave `applied` alone: the stamp stays different, so
        // the reload is retried on a later tick instead of being lost.
        return;
    }
    // The database's baseline is the reload's own read. The response
    // files' stamps predate it, so a rewrite landing meanwhile is seen next
    // tick; a response file this reload first named has none, so the
    // source reloads once more after settling, with one taken before it.
    tracked.applied = current;
    adopt_load(tracked);
    tracked.applied.responses.clear();
    tracked.reread = false;
    for(auto& response: project.cdb.response_files(tracked.id)) {
        auto it = known.find(response);
        if(it == known.end()) {
            tracked.reread = true;
        }
        tracked.applied.responses.push_back(it != known.end() ? it->second : stat_file(response));
    }
    LOG_INFO("Reloaded CDB from {}: {} added, {} removed, {} changed",
             project.cdb.source_path(tracked.id),
             diff->added.size(),
             diff->removed.size(),
             diff->changed.size());
    if(flips) {
        push_moved(shared, before, default_sources(shared), diff->changed);
    }
    append(delta, *diff);
}

void CDBWatcher::discover_into(Fid path_id, CDBDiff& found) {
    if(project.build.declares_sources() || !project.build.commands(path_id).empty()) {
        return;
    }
    auto path = project.file_table.resolve(path_id);
    if(!path::under(path, root)) {
        return;
    }
    // A registered database whose load failed so far (absent at startup,
    // unreadable at an earlier open) gets another try: with polling off
    // nothing else would.
    for(auto& database: compile_commands_above(path::parent_path(path), root)) {
        auto registered = project.cdb.find_source(database);
        if(registered && project.cdb.loaded(*registered)) {
            continue;
        }
        auto id = registered ? *registered : project.cdb.add_source(database);
        if(auto diff = project.cdb.reload_and_diff(id)) {
            LOG_INFO("Found compilation database: {}", database);
            append(found, *diff);
        }
        if(!registered) {
            track(id);
        }
    }
}

CDBDiff CDBWatcher::tick(llvm::ArrayRef<Fid> open_files, bool force) {
    CDBDiff delta;
    // Nothing declared: keep looking, so a database generated after
    // startup — at the root, in a new subdirectory, or above a file open
    // without one — is picked up. Declared sources are registered
    // (existing or not) and only watched.
    if(!project.build.declares_sources()) {
        for(auto& found: discover_compile_commands(root)) {
            auto id = project.cdb.add_source(found);
            if(llvm::none_of(sources,
                             [&](const TrackedSource& tracked) { return tracked.id == id; })) {
                // Baselined as missing: the fresh file is a change against
                // the never-loaded source and goes through the normal
                // settle-and-reload path.
                LOG_INFO("Found compilation database: {}", found);
                track(id);
            }
        }
        for(auto path_id: open_files) {
            discover_into(path_id, delta);
        }
    }
    for(auto& tracked: sources) {
        tick_source(tracked, force, delta);
    }
    return delta;
}

CDBDiff CDBWatcher::discover_around(Fid path_id) {
    CDBDiff found;
    discover_into(path_id, found);
    return found;
}

}  // namespace clice

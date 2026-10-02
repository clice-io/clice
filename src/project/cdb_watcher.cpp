#include "project/cdb_watcher.h"

#include <ranges>
#include <utility>

#include "support/logging.h"
#include "vfs/path.h"

#include "llvm/ADT/STLExtras.h"

namespace clice {

CDBWatcher::CDBWatcher(Project& project, CanonicalPath root) :
    project(project), root(std::move(root)) {
    for(std::size_t i = 0; i < project.cdb.source_count(); i += 1) {
        track(SourceID(i));
    }
}

CDBWatcher::Hashes CDBWatcher::loaded(SourceID id) const {
    return llvm::to_vector(
        llvm::map_range(project.cdb.inputs(id), [](auto& input) { return input.hash; }));
}

CDBWatcher::Hashes CDBWatcher::looked(const TrackedSource& tracked) {
    return llvm::to_vector(
        llvm::map_range(tracked.inputs, [](auto& input) { return input->hash; }));
}

void CDBWatcher::watch_inputs(TrackedSource& tracked) {
    tracked.inputs.truncate(1);
    for(auto& input: project.cdb.inputs(tracked.id).drop_front()) {
        tracked.inputs.push_back(
            project.file_table.disk.watch(project.file_table.resolve(input.file).str()));
    }
}

void CDBWatcher::track(SourceID id) {
    auto& tracked = sources.emplace_back(TrackedSource{
        .id = id,
        .applied = loaded(id),
        .inputs = {project.file_table.disk.watch(project.cdb.source_path(id).str())},
    });
    watch_inputs(tracked);
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
    auto current = looked(tracked);
    if(!force) {
        if(current == tracked.applied) {
            tracked.pending.reset();
            return;
        }
        if(tracked.pending != current) {
            // Generators rewrite the file in place; only act once the
            // content has held for two consecutive ticks (half-write guard).
            tracked.pending = std::move(current);
            return;
        }
    }
    // A forced tick reloads unconditionally: a spurious reload just yields
    // an empty diff.
    tracked.pending.reset();
    bool exists = tracked.inputs.front()->stamp.has_value();
    // A discovered database's presence ranks it (see Build::source_order):
    // the files whose default entry moves with it change command.
    bool flips = project.cdb.present(tracked.id) != exists && project.build.discovered(tracked.id);
    llvm::SmallVector<Fid> shared;
    llvm::SmallVector<std::optional<SourceID>> before;
    if(flips) {
        shared = shared_files(tracked.id);
        before = default_sources(shared);
    }
    if(!exists) {
        // Deleted — usually mid-regeneration. Keep serving the loaded
        // entries; the rewrite lands as the next observed change.
        tracked.applied = std::move(current);
        project.cdb.set_present(tracked.id, false);
        if(flips) {
            push_moved(shared, before, default_sources(shared), delta.changed);
        }
        return;
    }
    auto diff = project.cdb.reload_and_diff(tracked.id);
    if(!diff) {
        // Unreadable or unparsable right now (e.g. still locked by the
        // generator). Leave `applied` alone: the content stays different,
        // so the reload is retried on a later tick instead of being lost.
        return;
    }
    // The baseline is the reload's own reads: a rewrite landing meanwhile
    // is seen next tick.
    tracked.applied = loaded(tracked.id);
    watch_inputs(tracked);
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
    for(auto& database: compile_commands_above(path.parent(), root)) {
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

void CDBWatcher::discover(llvm::ArrayRef<Fid> open_files) {
    auto& disk = project.file_table.disk;
    if(!root_flag) {
        root_flag = disk.watch(root.str());
    }
    if(listed.empty() || root_flag->stamp != listed_at) {
        listed_at = root_flag->stamp;
        listed = database_places(root);
    }

    // In the order a startup discovery would register them, the nearest
    // database first above each open file.
    llvm::SmallVector<std::string> wanted;
    llvm::StringSet<> seen;
    auto want = [&](const Spelling& place) {
        if(seen.insert(place.str()).second) {
            wanted.push_back(place.str());
        }
    };
    for(auto& place: listed) {
        want(place);
    }
    for(auto path_id: open_files) {
        auto path = project.file_table.resolve(path_id);
        if(!project.build.commands(path_id).empty() || !path::under(path, root)) {
            continue;
        }
        path::walk_ancestors(path.parent(), root, [&](llvm::StringRef dir) {
            want(Spelling("compile_commands.json", Spelling::absolute(dir)));
            return true;
        });
    }

    llvm::SmallVector<std::string> unwanted;
    for(auto& entry: places) {
        if(!seen.contains(entry.getKey())) {
            unwanted.push_back(entry.getKey().str());
        }
    }
    for(auto& place: unwanted) {
        places.erase(place);
    }

    for(auto& place: wanted) {
        if(registered.contains(place)) {
            continue;
        }
        if(!places.contains(place)) {
            if(project.cdb.find_source(Spelling::absolute(place))) {
                registered.insert(place);
                continue;
            }
            places[place] = disk.watch(place);
        }
        if(!places[place]->stamp) {
            continue;
        }
        // Never loaded, so baselined unread: the fresh file is a change
        // against the never-loaded source and goes through the normal
        // settle-and-reload path.
        auto id = project.cdb.add_source(Spelling::absolute(place));
        if(llvm::none_of(sources, [&](const TrackedSource& tracked) { return tracked.id == id; })) {
            LOG_INFO("Found compilation database: {}", place);
            track(id);
        }
        registered.insert(place);
        places.erase(place);
    }
}

CDBDiff CDBWatcher::tick(llvm::ArrayRef<Fid> open_files, bool force) {
    // Nothing declared: keep watching the places a database may appear.
    // Declared sources are registered (existing or not) and only watched.
    if(!project.build.declares_sources()) {
        discover(open_files);
    }
    CDBDiff delta;
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

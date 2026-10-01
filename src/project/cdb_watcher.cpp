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

CDBWatcher::Hashes CDBWatcher::look(TrackedSource& tracked) {
    Hashes result;
    for(auto& input: tracked.inputs) {
        input.look();
        result.push_back(input.hash);
    }
    return result;
}

void CDBWatcher::watch_inputs(TrackedSource& tracked) {
    tracked.inputs.truncate(1);
    for(auto& input: project.cdb.inputs(tracked.id).drop_front()) {
        tracked.inputs.push_back({.path = project.file_table.resolve(input.file).str()});
    }
}

void CDBWatcher::track(SourceID id) {
    auto& tracked = sources.emplace_back(TrackedSource{
        .id = id,
        .applied = loaded(id),
        .inputs = {{.path = project.cdb.source_path(id).str()}},
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
    auto current = look(tracked);
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
    bool exists = !tracked.inputs.front().missing;
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
                // Never loaded, so baselined unread: the fresh file is a change against
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

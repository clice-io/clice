module;

#include "modules/prelude.h"

#include "support/logging.macros.h"

module clice;

import :index.database;
import :index.project_index;
import :index.tu_manifest;
import :project.command_snapshot;
import :project.hosting;
import :project.project;
import :support.json;
import :support.logging;
import :vfs.file_system;
import :vfs.path;

namespace clice {

namespace {

/// Entry hash of a file's default selection — the candidate-order winner,
/// or the default command covering a file without entries; empty when the
/// build does not compile it.
std::string selected_hash(Project& project, Fid file) {
    auto commands = project.build.commands(file);
    if(commands.empty()) {
        return {};
    }
    return project.cdb.entry_hash_hex(commands.front().config);
}

/// JSON layout of the persisted CDB snapshot (blob kind CDB): per source
/// file, the sorted canonical command hashes of its entries and a hash of
/// its matched config rules when the index state was last saved. A
/// standalone-indexed header gets an entry too (empty hashes): its own
/// matched rules plus the host source whose command its index borrowed.
/// Paths are the database's names (ProjectIndex::portable).
struct CDBSnapshotEntry {
    std::string file;
    std::vector<std::string> hashes;

    /// Entry hash of the file's default selection (the candidate-order
    /// winner, or the default command covering a file without entries).
    /// The hash multiset alone cannot see an offline flip of the winner —
    /// candidates unchanged, selection changed.
    std::string selected;

    /// The databases that listed the file, so a later session can tell a
    /// removal from a database that failed to load.
    std::vector<std::string> sources;

    std::string rules;
    std::string host;

    /// Entry hash of the host's default selection. A host that is not
    /// itself indexed (a default-command unit under `index = false`) has
    /// no entry of its own to record its command in.
    std::string host_selected;
};

struct CommandSnapshotBlob {
    std::vector<CDBSnapshotEntry> entries;

    /// The files recorded as provisional members (Build::recorded), sorted.
    std::vector<std::string> provisional;
};

/// clice.toml rules change the effective indexing command without touching
/// the CDB entry, so the snapshot must cover their edits too — an offline
/// rule edit is as stale-making as an offline command edit. A file without
/// candidates that a rule's default command covers records that command's
/// identity as its selection, so an offline edit of the command text is
/// caught the same way.
CommandSnapshotBlob build_snapshot(Project& project,
                                   const llvm::DenseMap<Fid, Fid>& header_hosts,
                                   llvm::ArrayRef<Fid> standalone_debt) {
    CommandSnapshotBlob snapshot;
    for(auto& bucket: project.cdb.command_hash_snapshot()) {
        auto path_id = bucket.first;
        auto candidates = project.build.entries(path_id);
        if(candidates.empty()) {
            // Entries only inactive configurations declare: not compiled
            // by this view, so not part of its identity.
            continue;
        }
        auto file = project.file_table.resolve(path_id);
        auto rules = project.build.edit_hash(file);
        // In build order, which registration order — the order databases
        // were discovered in — must not leak into: the sequence is the
        // file's command identity across sessions.
        std::vector<std::string> hashes;
        std::vector<std::string> sources;
        for(auto& candidate: candidates) {
            hashes.push_back(project.cdb.entry_hash_hex(candidate.config));
            auto database = project.cdb.source_path(candidate.source);
            // Commands added by hand come from no database a later session
            // could miss.
            if(database.empty()) {
                continue;
            }
            auto source = project.index.portable(database);
            if(!llvm::is_contained(sources, source)) {
                sources.push_back(std::move(source));
            }
        }
        snapshot.entries.push_back({
            .file = project.index.portable(file),
            .hashes = std::move(hashes),
            .selected = project.cdb.entry_hash_hex(candidates.front().config),
            .sources = std::move(sources),
            .rules = std::move(rules),
        });
    }
    // Standalone-indexed TUs have no CDB entry, yet their effective command
    // depends on the default command covering them or the host they borrow
    // from, and on the rules matching them and that host — all of it must
    // be snapshot to detect offline changes.
    auto add_standalone = [&](Fid tu) {
        auto file = project.file_table.resolve(tu);
        if(!project.build.entries(tu).empty()) {
            return;
        }
        std::string host;
        std::string host_selected;
        llvm::SmallVector<CanonicalRef, 2> edit_paths;
        if(auto host_it = header_hosts.find(tu); host_it != header_hosts.end()) {
            auto host_path = project.file_table.resolve(host_it->second);
            host = project.index.portable(host_path);
            host_selected = selected_hash(project, host_it->second);
            edit_paths.push_back(host_path);
        }
        edit_paths.push_back(file);
        auto rules = project.build.edit_hash(edit_paths);
        snapshot.entries.push_back({
            .file = project.index.portable(file),
            .selected = selected_hash(project, tu),
            .rules = std::move(rules),
            .host = std::move(host),
            .host_selected = std::move(host_selected),
        });
    };
    project.index.for_each_manifest([&](Fid tu, const index::TUManifest&) { add_standalone(tu); });
    // A dropped standalone TU whose rebuild has not finished keeps its entry:
    // with no manifest pin and no CDB entry, the snapshot is the only
    // record that an index is owed (reconcile's debt pass retries it).
    for(auto tu: standalone_debt) {
        add_standalone(tu);
    }
    for(auto file: project.build.recorded()) {
        snapshot.provisional.push_back(project.index.portable(project.file_table.resolve(file)));
    }
    // Deterministic bytes: a save decides "unchanged" by byte equality.
    std::ranges::sort(snapshot.entries, {}, &CDBSnapshotEntry::file);
    std::ranges::sort(snapshot.provisional);
    return snapshot;
}

/// The debt candidates the snapshot must record: standalone TUs with no
/// manifest pin and no CDB entry, for which the snapshot is the only record
/// that an index is owed.
llvm::SmallVector<Fid> standalone_of(Project& project, llvm::ArrayRef<Fid> candidates) {
    llvm::SmallVector<Fid> debt;
    llvm::DenseSet<Fid> seen;
    for(auto id: candidates) {
        if(project.index.held_manifest(id) || !project.build.entries(id).empty() ||
           !seen.insert(id).second) {
            continue;
        }
        debt.push_back(id);
    }
    return debt;
}

}  // namespace

CommandSnapshot::Remembered CommandSnapshot::remembered(const Project& project) {
    Remembered result;
    if(!project.database) {
        return result;
    }
    auto blob = project.database->read(index::BlobKind::CommandSnapshot, "cdb");
    CommandSnapshotBlob persisted;
    if(!blob ||
       !kota::codec::json::from_string(std::string_view(blob.buffer->getBuffer()), persisted)) {
        return result;
    }
    for(auto& entry: persisted.entries) {
        for(auto& source: entry.sources) {
            auto absolute = project.index.local(source);
            if(!llvm::is_contained(result.sources, absolute)) {
                result.sources.push_back(std::move(absolute));
            }
        }
    }
    for(auto& file: persisted.provisional) {
        result.provisional.push_back(project.index.local(file));
    }
    return result;
}

llvm::SmallVector<Fid> CommandSnapshot::hosted_by(Fid host) const {
    llvm::SmallVector<Fid> headers;
    for(auto& [header, recorded]: header_hosts) {
        if(recorded == host) {
            headers.push_back(header);
        }
    }
    return headers;
}

bool CommandSnapshot::owes_save(const Project& project) const {
    return dirty || project.build.recorded_generation() != serialized_records;
}

std::optional<std::string> CommandSnapshot::serialize(Project& project, llvm::ArrayRef<Fid> debt) {
    serialized_records = project.build.recorded_generation();
    auto json = kota::codec::json::to_string<PathJsonConfig>(
        build_snapshot(project, header_hosts, standalone_of(project, debt)));
    if(!json) {
        LOG_WARN("Failed to serialize the CDB snapshot: {}", json.error().to_string());
    }
    if(!json || *json == persisted_bytes) {
        dirty = false;
        return std::nullopt;
    }
    return std::move(*json);
}

void CommandSnapshot::reconcile(Project& project, index::IndexReport& report) {
    auto blob = project.database->read(index::BlobKind::CommandSnapshot, "cdb");
    if(!blob) {
        // Unknown baseline: nothing to diff against. Dirty the snapshot so
        // the next save recreates it even when it commits nothing else —
        // after a crash that lost only the command snapshot, waiting for an
        // unrelated dirtying merge would leave offline command edits
        // undetectable across every following session.
        dirty = true;
        return;
    }
    CommandSnapshotBlob persisted;
    if(!kota::codec::json::from_string(std::string_view(blob.buffer->getBuffer()), persisted)) {
        LOG_ERROR("Index cache at {} cannot tell which commands built it; reindexing every file",
                  std::string_view(project.config.project.cache_dir));
        dirty = true;
        llvm::SmallVector<Fid> units;
        project.index.for_each_manifest(
            [&](Fid tu, const index::TUManifest&) { units.push_back(tu); });
        for(auto unit: units) {
            project.index.drop(unit, report);
            report.add_reindex(unit);
        }
        return;
    }
    persisted_bytes = blob.buffer->getBuffer().str();

    llvm::StringMap<const CDBSnapshotEntry*> before;
    for(auto& entry: persisted.entries) {
        before[entry.file] = &entry;
    }
    auto& project_index = project.index;
    auto file_of = [&](llvm::StringRef name) {
        return project.file_table.intern(project_index.local(name));
    };
    llvm::DenseSet<Fid> cdb_ids;
    llvm::SmallVector<Fid> changed_ids;
    auto snapshot = build_snapshot(project, header_hosts, {});
    for(auto& entry: snapshot.entries) {
        if(entry.hashes.empty()) {
            continue;
        }
        auto server_id = file_of(entry.file);
        cdb_ids.insert(server_id);
        auto it = before.find(entry.file);
        // `selected` guards the offline winner flip: the candidate multiset
        // can survive a reload that still changes which entry is the
        // default selection.
        if(it != before.end() && it->second->hashes == entry.hashes &&
           it->second->selected == entry.selected && it->second->rules == entry.rules &&
           it->second->sources == entry.sources) {
            continue;
        }
        changed_ids.push_back(server_id);
        if(!project_index.held_manifest(server_id)) {
            continue;
        }
        LOG_INFO("Compile command changed since the last session; reindexing {}",
                 project.file_table.resolve(server_id));
        project_index.drop(server_id, report);
        report.add_reindex(server_id);
    }

    // A standalone-indexed header borrowed a host source's command and
    // applied its own matched rules on top, so an offline change to either
    // staled its index exactly like the live CDB path's hosted-header
    // invalidation. A header whose recorded host survives unchanged and
    // still includes it is pinned fresh; one with no recorded host (older
    // snapshot) falls back to the include-reachability approximation below.
    llvm::DenseSet<Fid> pinned_fresh;
    llvm::DenseSet<Fid> retired;
    for(auto& entry: snapshot.entries) {
        if(!entry.hashes.empty()) {
            continue;
        }
        auto it = before.find(entry.file);
        if(it == before.end()) {
            // Not in the persisted snapshot: the next save adopts it, and
            // offline changes against an unknown baseline are undetectable
            // anyway.
            continue;
        }
        auto& old = *it->second;
        auto server_id = file_of(entry.file);
        if(!old.hashes.empty()) {
            // Its entries vanished. A default command that still covers it
            // is a command change; otherwise the retirement pass below
            // decides whether the index leaves.
            if(!entry.selected.empty()) {
                LOG_INFO("Compile command changed since the last session; reindexing {}",
                         project.file_table.resolve(server_id));
                project_index.drop(server_id, report);
                report.add_reindex(server_id);
            }
            continue;
        }
        // Hosts are only known once a header has been resolved this
        // session; until then the recorded host stands in, or every host
        // edit would read as a rule change at each start.
        std::string rules = entry.rules;
        std::string host_selected = entry.host_selected;
        if(entry.host.empty() && !old.host.empty()) {
            auto host_id = file_of(old.host);
            CanonicalRef paths[] = {project.file_table.resolve(host_id),
                                    project.file_table.resolve(server_id)};
            rules = project.build.edit_hash(paths);
            host_selected = selected_hash(project, host_id);
        }
        if(old.rules != rules || old.selected != entry.selected) {
            // The command that covered it — a rule's default command, or the
            // one a provisional member borrowed — is gone and no host
            // confirms it: the build stopped compiling it, so its index
            // leaves rather than being rebuilt under the builtin fallback.
            if(!old.selected.empty() && entry.selected.empty() && old.host.empty()) {
                LOG_INFO("The build no longer compiles {}; dropping its index",
                         project.file_table.resolve(server_id));
                project_index.drop(server_id, report);
                retired.insert(server_id);
                continue;
            }
            LOG_INFO("Compile command or rules changed since the last session; reindexing {}",
                     project.file_table.resolve(server_id));
            project_index.drop(server_id, report);
            report.add_reindex(server_id);
            changed_ids.push_back(server_id);
            continue;
        }
        if(old.host.empty()) {
            continue;
        }
        auto host_id = file_of(old.host);
        if(project.build.commands(host_id).empty() || llvm::is_contained(changed_ids, host_id) ||
           old.host_selected != host_selected) {
            LOG_INFO("Host compile command changed since the last session; reindexing {}",
                     project.file_table.resolve(server_id));
            project_index.drop(server_id, report);
            report.add_reindex(server_id);
            continue;
        }
        // The dependency scan preceding this load saw the offline edits, so
        // a recorded host that the build no longer ranks first — it stopped
        // including the header, or a rule change moved another database
        // ahead — cannot confirm the borrowed command any more. Keep the
        // index serving (last known good, like the vanished-entry case
        // above) while a rebuild re-selects a host; a Fallback resolution
        // then changes nothing. The retained index was still built through
        // the old host, so keep that association until a finished rebuild
        // overwrites it — an empty host persisted after a Fallback or
        // failed rebuild would hit the `old.host.empty()` gate next session
        // and never retry.
        auto current = default_host(project, server_id);
        if(!current || current->file != host_id) {
            LOG_INFO("Default host of {} changed since the last session; reindexing",
                     project.file_table.resolve(server_id));
            header_hosts[server_id] = host_id;
            report.add_reindex(server_id);
            continue;
        }
        header_hosts[server_id] = host_id;
        pinned_fresh.insert(server_id);
    }

    // A file no database lists any more: when every database that listed
    // it loaded fine this session, the build stopped compiling it and its
    // index leaves, as the live reload's removed branch does; a database
    // that failed to load keeps its last-known entries serving. One the
    // configuration stopped declaring is gone on purpose; one discovery no
    // longer finds may come back.
    bool declared = project.build.declares_sources();
    // Under discovery a database that is no longer registered was either
    // replaced by one that loaded, or merely vanished and may come back.
    bool replaced = false;
    for(std::uint32_t i = 0; !declared && i < project.cdb.source_count(); i += 1) {
        replaced |= project.cdb.loaded(SourceID(i));
    }
    for(auto& old: persisted.entries) {
        if(old.hashes.empty() || old.sources.empty()) {
            continue;
        }
        auto server_id = file_of(old.file);
        if(!project.build.commands(server_id).empty()) {
            continue;
        }
        bool healthy = llvm::all_of(old.sources, [&](const std::string& source) {
            auto id = project.cdb.find_source(project_index.local(source));
            return id ? project.cdb.loaded(*id) : declared || replaced;
        });
        if(!healthy) {
            continue;
        }
        LOG_INFO("No compilation database lists {} any more; dropping its index",
                 project.file_table.resolve(server_id));
        project_index.drop(server_id, report);
    }

    // A persisted standalone entry whose file has no manifest is recorded
    // debt: its index was dropped for a command or rule change and no
    // rebuild has finished since — with no manifest pin and no CDB entry,
    // nothing else would ever retry it. Re-enqueue while the file exists;
    // a vanished file's debt dies with its entry at the next save.
    for(auto& old: persisted.entries) {
        if(!old.hashes.empty() || !project.build.entries(file_of(old.file)).empty()) {
            continue;
        }
        auto server_id = file_of(old.file);
        if(retired.contains(server_id) || project_index.held_manifest(server_id) ||
           !vfs::exists(project.file_table.resolve(server_id))) {
            continue;
        }
        LOG_INFO("Index owed from the last session; reindexing {}",
                 project.file_table.resolve(server_id));
        report.add_reindex(server_id);
    }
    if(changed_ids.empty()) {
        return;
    }

    llvm::SmallVector<Fid> hosted;
    project_index.for_each_manifest([&](Fid tu, const index::TUManifest&) {
        if(cdb_ids.contains(tu) || pinned_fresh.contains(tu)) {
            return;
        }
        if(llvm::any_of(changed_ids, [&](Fid host) {
               return !project.dep_graph.find_include_chain(host, tu).empty();
           })) {
            hosted.push_back(tu);
        }
    });
    for(auto header_id: hosted) {
        LOG_INFO("Host compile command changed since the last session; reindexing {}",
                 project.file_table.resolve(header_id));
        project_index.drop(header_id, report);
        report.add_reindex(header_id);
    }
}

}  // namespace clice

module clice;

import :command.file_kind;
import :index.database;
import :index.project_index;
import :project.command_snapshot;
import :project.project;
import :support.json;
import :support.logging;
import :vfs.path;

namespace clice {

namespace {

/// Command hash of a file's default selection — the candidate-order winner,
/// or the default command covering a file without entries; empty when the
/// build does not compile it.
std::string selected_hash(Project& project, Fid file) {
    auto commands = project.build.commands(file);
    if(commands.empty()) {
        return {};
    }
    return project.commands.command_hash_hex(commands.front().command);
}

/// JSON layout of the persisted command snapshot (blob kind
/// CommandSnapshot): per source file, the sorted canonical command hashes of
/// its entries and a hash of its matched rules when the index state
/// was last saved. A unit without entries gets an entry too (empty
/// hashes): the command covering it and its matched rules. Paths are the
/// database's names (ProjectIndex::portable).
struct Entry {
    std::string file;

    std::vector<std::string> hashes;

    /// Command hash of the file's default selection (the candidate-order
    /// winner, or the default command covering a file without entries).
    /// The hash multiset alone cannot see an offline flip of the winner —
    /// candidates unchanged, selection changed.
    std::string selected;

    /// The databases that listed the file, so a later session can tell a
    /// removal from a database that failed to load.
    std::vector<std::string> sources;

    std::string rules;
};

struct Blob {
    std::vector<Entry> entries;

    /// The files recorded as provisional members (Build::recorded), sorted.
    std::vector<std::string> provisional;
};

/// clice.toml rules change the effective indexing command without touching
/// the CDB entry, so the snapshot must cover their edits too — an offline
/// rule edit is as stale-making as an offline command edit. A file without
/// candidates that a rule's default command covers records that command's
/// identity as its selection, so an offline edit of the command text is
/// caught the same way.
Blob build_snapshot(Project& project, llvm::ArrayRef<Fid> unlisted_debt) {
    Blob snapshot;
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
            hashes.push_back(project.commands.command_hash_hex(candidate.command));
            auto database = project.cdb.source_path(candidate.source);
            // Commands added by hand come from no database a later session
            // could miss.
            if(database.empty()) {
                continue;
            }
            auto source = project.index.portable(database);
            if(!ranges::contains(sources, source)) {
                sources.push_back(std::move(source));
            }
        }
        snapshot.entries.push_back({
            .file = project.index.portable(file),
            .hashes = std::move(hashes),
            .selected = project.commands.command_hash_hex(candidates.front().command),
            .sources = std::move(sources),
            .rules = std::move(rules),
        });
    }
    // TUs no database lists: their effective command depends on the
    // default command covering them and on the rules matching them — all
    // of it must be snapshot to detect offline changes.
    auto add_unlisted = [&](Fid tu) {
        auto file = project.file_table.resolve(tu);
        if(!project.build.entries(tu).empty()) {
            return;
        }
        snapshot.entries.push_back({
            .file = project.index.portable(file),
            .selected = selected_hash(project, tu),
            .rules = project.build.edit_hash(file),
        });
    };
    project.index.for_each_unit([&](Fid tu) { add_unlisted(tu); });
    // A dropped unlisted TU whose rebuild has not finished keeps its entry:
    // with no manifest and no CDB entry, the snapshot is the only
    // record that an index is owed (reconcile's debt pass retries it).
    for(auto tu: unlisted_debt) {
        add_unlisted(tu);
    }
    for(auto file: project.build.recorded()) {
        snapshot.provisional.push_back(project.index.portable(project.file_table.resolve(file)));
    }
    // Deterministic bytes: a save decides "unchanged" by byte equality.
    ranges::sort(snapshot.entries, {}, &Entry::file);
    ranges::sort(snapshot.provisional);
    return snapshot;
}

/// Whether `file` is a header the build compiles as no unit of its own:
/// the units reading it index it, while an older clice indexed one on its
/// own when it was opened before them.
bool lone_header(Project& project, Fid file) {
    return !project.build.unit(file) && is_header_path(project.file_table.resolve(file));
}

/// The debt candidates the snapshot must record: TUs with no manifest and
/// no CDB entry, for which the snapshot is the only record that an index
/// is owed.
llvm::SmallVector<Fid> unlisted_of(Project& project, llvm::ArrayRef<Fid> candidates) {
    llvm::SmallVector<Fid> debt;
    llvm::DenseSet<Fid> seen;
    for(auto id: candidates) {
        if(project.index.indexed(id) || !project.build.entries(id).empty() ||
           lone_header(project, id) || !seen.insert(id).second) {
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
    Blob persisted;
    if(!blob || !json::from_string(std::string_view(blob.buffer->getBuffer()), persisted)) {
        return result;
    }
    for(auto& entry: persisted.entries) {
        for(auto& source: entry.sources) {
            auto absolute = project.index.local(source);
            if(!ranges::contains(result.sources, absolute)) {
                result.sources.push_back(std::move(absolute));
            }
        }
    }
    for(auto& file: persisted.provisional) {
        result.provisional.push_back(project.index.local(file));
    }
    return result;
}

bool CommandSnapshot::owes_save(const Project& project) const {
    return dirty || project.build.recorded_generation() != serialized_records;
}

std::optional<std::string> CommandSnapshot::serialize(Project& project, llvm::ArrayRef<Fid> debt) {
    serialized_records = project.build.recorded_generation();
    auto json =
        json::to_string<PathJsonConfig>(build_snapshot(project, unlisted_of(project, debt)));
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
    auto& project_index = project.index;
    auto rebuild = [&](Fid unit) {
        project_index.drop(unit, report);
        report.add_reindex(unit);
    };
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
    Blob persisted;
    if(!json::from_string(std::string_view(blob.buffer->getBuffer()), persisted)) {
        LOG_ERROR("Index cache at {} cannot tell which commands built it; reindexing every file",
                  std::string_view(project.settings.project.cache_dir));
        dirty = true;
        llvm::SmallVector<Fid> units;
        project_index.for_each_unit([&](Fid tu) { units.push_back(tu); });
        ranges::for_each(units, rebuild);
        return;
    }
    persisted_bytes = blob.buffer->getBuffer().str();

    llvm::StringMap<const Entry*> before;
    for(auto& entry: persisted.entries) {
        before[entry.file] = &entry;
    }
    auto file_of = [&](llvm::StringRef name) {
        return project.file_table.intern(project_index.local(name));
    };
    auto snapshot = build_snapshot(project, {});
    for(auto& entry: snapshot.entries) {
        if(entry.hashes.empty()) {
            continue;
        }
        auto server_id = file_of(entry.file);
        auto it = before.find(entry.file);
        // `selected` guards the offline winner flip: the candidate multiset
        // can survive a reload that still changes which entry is the
        // default selection.
        if(it != before.end() && it->second->hashes == entry.hashes &&
           it->second->selected == entry.selected && it->second->rules == entry.rules) {
            // Listed by other databases, the same command builds the same
            // index: only the snapshot follows the databases now listing it.
            dirty = dirty || it->second->sources != entry.sources;
            continue;
        }
        if(!project_index.indexed(server_id)) {
            continue;
        }
        LOG_INFO("Compile command changed since the last session; reindexing {}",
                 project.file_table.resolve(server_id));
        rebuild(server_id);
    }

    // A TU without entries is stale when the command covering it — a
    // rule's default command, or the one a provisional member borrows — or
    // its rules changed offline; one whose command is gone leaves the
    // index rather than being rebuilt under the builtin fallback, and so
    // does a header an older clice indexed on its own.
    llvm::DenseSet<Fid> retired;
    const Entry unknown;
    for(auto& entry: snapshot.entries) {
        if(!entry.hashes.empty()) {
            continue;
        }
        auto it = before.find(entry.file);
        auto server_id = file_of(entry.file);
        if(it != before.end() && !it->second->hashes.empty()) {
            // Its entries vanished. A default command that still covers it
            // is a command change; otherwise the retirement pass below
            // decides whether the index leaves.
            if(!entry.selected.empty()) {
                LOG_INFO("Compile command changed since the last session; reindexing {}",
                         project.file_table.resolve(server_id));
                rebuild(server_id);
            }
            continue;
        }
        auto& old = it != before.end() ? *it->second : unknown;
        bool dropped = !old.selected.empty() && entry.selected.empty();
        if(dropped || lone_header(project, server_id)) {
            LOG_INFO("The build no longer compiles {}; dropping its index",
                     project.file_table.resolve(server_id));
            project_index.drop(server_id, report);
            retired.insert(server_id);
            continue;
        }
        // Not in the persisted snapshot: the next save adopts it, and
        // offline changes against an unknown baseline are undetectable
        // anyway.
        if(it == before.end()) {
            continue;
        }
        if(old.rules != entry.rules || old.selected != entry.selected) {
            LOG_INFO("Compile command or rules changed since the last session; reindexing {}",
                     project.file_table.resolve(server_id));
            rebuild(server_id);
        }
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
        bool healthy = ranges::all_of(old.sources, [&](const std::string& source) {
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

    // A persisted entry of a TU without entries whose file has no manifest
    // is recorded debt: its index was dropped for a command or rule change
    // and no rebuild has finished since — with no manifest and no CDB
    // entry, nothing else would ever retry it. Re-enqueue while the file
    // exists; a vanished file's debt dies with its entry at the next save.
    for(auto& old: persisted.entries) {
        if(!old.hashes.empty() || !project.build.entries(file_of(old.file)).empty()) {
            continue;
        }
        auto server_id = file_of(old.file);
        if(retired.contains(server_id) || lone_header(project, server_id) ||
           project_index.indexed(server_id) || !fs::exists(project.file_table.resolve(server_id))) {
            continue;
        }
        LOG_INFO("Index owed from the last session; reindexing {}",
                 project.file_table.resolve(server_id));
        report.add_reindex(server_id);
    }
}

}  // namespace clice

module clice;

import :command.file_kind;
import :index.database;
import :index.project_index;
import :project.build_snapshot;
import :project.command;
import :project.discovery;
import :project.project;
import :support.json;
import :support.logging;
import :vfs.path;

namespace clice {

namespace {

/// JSON layout of the persisted build snapshot (blob kind BuildSnapshot):
/// per source file, the databases that listed it when the index state was
/// last saved, and the provisional members. Paths are the database's names
/// (ProjectIndex::portable). A snapshot written before format 53 also holds
/// each file's command hashes, which the reader ignores.
struct Entry {
    std::string file;

    /// The databases that listed the file, so a later session can tell a
    /// removal from a database that failed to load.
    std::vector<std::string> sources;
};

struct Blob {
    std::vector<Entry> entries;

    /// The files recorded as provisional members (Build::recorded), sorted.
    std::vector<std::string> provisional;
};

Blob build_snapshot(Project& project) {
    Blob snapshot;
    auto entries = project.cdb.entries();
    for(std::size_t i = 0; i < entries.size();) {
        auto path_id = entries[i].file;
        while(i < entries.size() && entries[i].file == path_id) {
            i += 1;
        }
        // In build order, which registration order — the order databases
        // were discovered in — must not leak into. Entries only inactive
        // configurations declare are not compiled by this view.
        std::vector<std::string> sources;
        for(auto& candidate: project.build.entries(path_id)) {
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
        if(sources.empty()) {
            continue;
        }
        snapshot.entries.push_back({
            .file = project.index.portable(project.file_table.resolve(path_id)),
            .sources = std::move(sources),
        });
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

}  // namespace

BuildSnapshot::Remembered BuildSnapshot::remembered(const Project& project) {
    Remembered result;
    if(!project.database) {
        return result;
    }
    auto blob = project.database->read(index::BlobKind::BuildSnapshot, "cdb");
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

bool BuildSnapshot::owes_save(const Project& project) const {
    return dirty || project.build.recorded_generation() != serialized_records;
}

std::optional<std::string> BuildSnapshot::serialize(Project& project) {
    serialized_records = project.build.recorded_generation();
    auto json = json::to_string<PathJsonConfig>(build_snapshot(project));
    if(!json) {
        LOG_WARN("Failed to serialize the build snapshot: {}", json.error().to_string());
    }
    if(!json || *json == persisted_bytes) {
        dirty = false;
        return std::nullopt;
    }
    return std::move(*json);
}

void BuildSnapshot::reconcile(Project& project, index::IndexReport& report) {
    auto& project_index = project.index;
    // Without the persisted snapshot nothing tells which databases listed a
    // unit, so none leaves the index. Dirty the snapshot so the next save
    // recreates it even when it commits nothing else: waiting for an
    // unrelated dirtying merge would leave every following session unable
    // to tell a removed unit.
    std::optional<Blob> persisted;
    if(auto blob = project.database->read(index::BlobKind::BuildSnapshot, "cdb")) {
        if(Blob read; json::from_string(std::string_view(blob.buffer->getBuffer()), read)) {
            persisted = std::move(read);
            persisted_bytes = blob.buffer->getBuffer().str();
        } else {
            LOG_ERROR(
                "Index cache at {} cannot tell which databases listed its files; keeping every "
                "unit the build no longer compiles",
                std::string_view(project.settings.project.cache_dir));
        }
    }
    llvm::StringMap<const Entry*> before;
    if(persisted) {
        for(auto& entry: persisted->entries) {
            before[entry.file] = &entry;
        }
        // Listed by other databases, a unit compiles as it did: only the
        // snapshot follows the databases now listing it.
        for(auto& entry: build_snapshot(project).entries) {
            if(auto it = before.find(entry.file);
               it != before.end() && it->second->sources != entry.sources) {
                dirty = true;
            }
        }
    } else {
        dirty = true;
    }

    // One the configuration stopped declaring is gone on purpose, and so is
    // one in a project nested in the root; one discovery no longer finds
    // may come back. Under discovery a database that is no longer
    // registered was either replaced by one that loaded, or merely vanished
    // and may come back.
    bool declared = project.build.declares_sources();
    bool replaced = false;
    for(std::uint32_t i = 0; !declared && i < project.cdb.source_count(); i += 1) {
        replaced |= project.cdb.loaded(SourceID(i));
    }
    auto healthy = [&](const Entry& entry) {
        return ranges::all_of(entry.sources, [&](const std::string& source) {
            auto path = project_index.local(source);
            if(auto id = project.cdb.find_source(path)) {
                return project.cdb.loaded(*id);
            }
            return declared || replaced ||
                   !nested_project(project.root, CanonicalPath(path)).empty();
        });
    };

    llvm::SmallVector<Fid> units;
    project_index.for_each_unit([&](Fid unit) { units.push_back(unit); });
    for(auto unit: units) {
        auto path = project.file_table.resolve(unit);
        if(lone_header(project, unit)) {
            LOG_INFO("The build no longer compiles {}; dropping its index", path);
            project_index.drop(unit, report);
            continue;
        }
        auto command = command_identity(project, unit);
        if(command == 0) {
            if(!persisted) {
                continue;
            }
            // A unit a database listed leaves when every database that
            // listed it loaded fine this session, as the live reload's
            // removed branch does; one that failed to load keeps its
            // last-known entries serving. Otherwise the default command
            // that covered it is gone, unless none ever did.
            if(auto it = before.find(project_index.portable(path));
               it != before.end() && !it->second->sources.empty()) {
                if(!healthy(*it->second)) {
                    continue;
                }
                LOG_INFO("No compilation database lists {} any more; dropping its index", path);
            } else if(project_index.indexed_command(unit) != 0) {
                LOG_INFO("The build no longer compiles {}; dropping its index", path);
            } else {
                continue;
            }
            project_index.drop(unit, report);
            continue;
        }
        // A unit deleted meanwhile keeps its shard behind (queries withhold
        // it) with nothing left to reindex.
        if(command != project_index.indexed_command(unit) && fs::exists(path)) {
            LOG_INFO("Compile command changed since the last session; reindexing {}", path);
            report.add_reindex(unit);
        }
    }
}

}  // namespace clice

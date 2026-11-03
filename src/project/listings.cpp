module clice;

import :command.file_kind;
import :index.database;
import :index.project_index;
import :project.discovery;
import :project.listings;
import :project.project;
import :support.json;
import :support.logging;
import :vfs.path;

namespace clice {

void SavedListings::read(index::BlobDatabase& database) {
    auto blob = database.read(index::BlobKind::Listings, "cdb");
    if(!blob) {
        return;
    }
    if(Blob read; json::from_string(std::string_view(blob.buffer->getBuffer()), read)) {
        read_blob = std::move(read);
        persisted_bytes = blob.buffer->getBuffer().str();
    } else {
        unparsed = true;
    }
}

llvm::SmallVector<Spelling> SavedListings::databases(const index::ProjectIndex& index) const {
    llvm::SmallVector<Spelling> result;
    if(!read_blob) {
        return result;
    }
    for(auto& entry: read_blob->entries) {
        for(auto& source: entry.sources) {
            auto absolute = index.local(source);
            if(!ranges::contains(result, absolute)) {
                result.push_back(std::move(absolute));
            }
        }
    }
    return result;
}

std::vector<Spelling> SavedListings::provisional(const index::ProjectIndex& index) const {
    std::vector<Spelling> result;
    if(read_blob) {
        for(auto& file: read_blob->provisional) {
            result.push_back(index.local(file));
        }
    }
    return result;
}

std::optional<std::string> SavedListings::serialize(const Blob& live, std::uint64_t provisional) {
    serialized_provisional = provisional;
    auto json = json::to_string(live, path_json);
    if(!json) {
        LOG_WARN("Failed to serialize the build's listings: {}", json.error().to_string());
    }
    if(!json || *json == persisted_bytes) {
        dirty = false;
        return std::nullopt;
    }
    return std::move(*json);
}

SavedListings::Blob Project::listed() {
    SavedListings::Blob listed;
    auto all = cdb.entries();
    for(std::size_t i = 0; i < all.size();) {
        auto path_id = all[i].file;
        while(i < all.size() && all[i].file == path_id) {
            i += 1;
        }
        // In build order, which registration order — the order databases
        // were discovered in — must not leak into. Entries only inactive
        // configurations declare are not compiled by this view.
        std::vector<std::string> sources;
        for(auto& candidate: entries(path_id)) {
            auto database = cdb.source_path(candidate.source);
            // Commands added by hand come from no database a later session
            // could miss.
            if(database.empty()) {
                continue;
            }
            auto source = index.portable(database);
            if(!ranges::contains(sources, source)) {
                sources.push_back(std::move(source));
            }
        }
        if(sources.empty()) {
            continue;
        }
        listed.entries.push_back({
            .file = index.portable(file_table.resolve(path_id)),
            .sources = std::move(sources),
        });
    }
    for(auto file: provisional.files()) {
        listed.provisional.push_back(index.portable(file_table.resolve(file)));
    }
    // Deterministic bytes: a save decides "unchanged" by byte equality.
    ranges::sort(listed.entries, {}, &SavedListings::Listing::file);
    ranges::sort(listed.provisional);
    return listed;
}

void Project::reconcile_index(index::IndexReport& report) {
    // Without the persisted listings nothing tells which databases listed a
    // unit, so none leaves the index. They are owed a save even when it
    // commits nothing else: waiting for an unrelated dirtying merge would
    // leave every following session unable to tell a removed unit.
    auto* persisted = listings.persisted();
    if(listings.unreadable()) {
        LOG_ERROR(
            "Index cache at {} cannot tell which databases listed its files; keeping every "
            "unit the build no longer compiles",
            std::string_view(settings.project.cache_dir));
    }
    llvm::StringMap<const SavedListings::Listing*> before;
    if(persisted) {
        for(auto& entry: persisted->entries) {
            before[entry.file] = &entry;
        }
        // Listed by other databases, a unit compiles as it did: only the
        // listings follow the databases now listing it.
        for(auto& entry: listed().entries) {
            if(auto it = before.find(entry.file);
               it != before.end() && it->second->sources != entry.sources) {
                listings.owe_save();
            }
        }
    } else {
        listings.owe_save();
    }

    // One the configuration stopped declaring is gone on purpose, and so is
    // one in a project nested in the root; one discovery no longer finds
    // may come back. Under discovery a database that is no longer
    // registered was either replaced by one that loaded, or merely vanished
    // and may come back.
    bool declared = rules.declares_sources();
    bool replaced = false;
    for(std::uint32_t i = 0; !declared && i < cdb.source_count(); i += 1) {
        replaced |= cdb.loaded(SourceID(i));
    }
    auto healthy = [&](const SavedListings::Listing& entry) {
        return ranges::all_of(entry.sources, [&](const std::string& source) {
            auto path = index.local(source);
            if(auto id = cdb.find_source(path)) {
                return cdb.loaded(*id);
            }
            return declared || replaced || !nested_project(root, CanonicalPath(path)).empty();
        });
    };

    llvm::SmallVector<Fid> units;
    index.for_each_unit([&](Fid unit) { units.push_back(unit); });
    for(auto unit: units) {
        auto path = file_table.resolve(unit);
        // An index built before the rule would otherwise serve on: the
        // background index never rebuilds an excluded unit.
        if(!rules.indexable(path)) {
            LOG_INFO("A rule keeps {} out of the index; dropping its entries", path);
            index.drop(unit, report);
            continue;
        }
        // A header the units reading it index, which an older clice indexed
        // on its own when it was opened before them.
        if(!is_unit(unit) && is_header_path(path)) {
            LOG_INFO("The build no longer compiles {}; dropping its index", path);
            index.drop(unit, report);
            continue;
        }
        auto command = command_identity(unit);
        if(command == 0) {
            if(!persisted) {
                continue;
            }
            // A unit a database listed leaves when every database that
            // listed it loaded fine this session, as the live reload's
            // removed branch does; one that failed to load keeps its
            // last-known entries serving. Otherwise the default command
            // that covered it is gone.
            if(auto it = before.find(index.portable(path));
               it != before.end() && !it->second->sources.empty()) {
                if(!healthy(*it->second)) {
                    continue;
                }
                LOG_INFO("No compilation database lists {} any more; dropping its index", path);
            } else {
                LOG_INFO("The build no longer compiles {}; dropping its index", path);
            }
            index.drop(unit, report);
            continue;
        }
        // A unit deleted meanwhile keeps its shard behind (queries withhold
        // it) with nothing left to reindex.
        if(command != index.indexed_command(unit) && fs::exists(path)) {
            LOG_INFO("Compile command changed since the last session; reindexing {}", path);
            index.mark_outdated(unit);
            report.add_reindex(unit);
        }
    }
}

}  // namespace clice

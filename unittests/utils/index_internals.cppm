module clice:unittests.utils.index_internals;

import :index.project_index;
import :index.shard;
import :support.prelude;
import :vfs.ids;

namespace clice::testing {

/// Index state a test sets up or looks at by hand, past the public API.
struct IndexInternals {
    /// Install `shard` as `file`'s, as a merge would; `written` has the next
    /// save write it.
    static void plant_shard(index::ProjectIndex& index,
                            Fid file,
                            index::Shard shard,
                            bool written = false) {
        auto& record = index.change_file(file);
        record.generation = shard.content_hash();
        record.stored = shard.variants();
        index.shards[file] = std::move(shard);
        if(written) {
            index.changed_shards.insert(file);
        }
    }

    /// Retire `file`'s shard, as a compaction finding no live variant does.
    static void retire_shard(index::ProjectIndex& index, Fid file) {
        index.shards[file] = index::Shard();
        index.changed_shards.erase(file);
        index.retired_shards.insert(file);
        auto& record = index.change_file(file);
        record.generation = 0;
        record.stored.clear();
    }

    /// Whether `tu` is outdated (ProjectIndex::mark_outdated).
    static bool outdated(const index::ProjectIndex& index, Fid tu) {
        return index.outdated.contains(tu);
    }

    /// Mark `tu` outdated since `when` (ms since the epoch), as
    /// mark_outdated does at the present.
    static void outdated_since(index::ProjectIndex& index, Fid tu, std::int64_t when) {
        index.outdated[tu] = when;
    }

    /// The id the database names `file` by, handed out on first use.
    static std::uint32_t path_id(index::ProjectIndex& index, Fid file) {
        return index.path_id(file);
    }

    /// The units the header counts a manifest of.
    static std::uint32_t unit_count(const index::ProjectIndex& index) {
        return index.header.unit_count;
    }

    /// The key of `file`'s records, its shards' and its manifest's.
    static std::string file_key(const index::ProjectIndex& index, Fid file) {
        return index.file_key(file);
    }

    /// Whether the file records the manifests derive (contributions, absent
    /// lookups, importers) hold no entry of any unit.
    static bool derived_tables_empty(const index::ProjectIndex& index) {
        return ranges::all_of(index.dependent_records.changed, [](const auto& entry) {
            return !entry.second || entry.second->empty();
        });
    }
};

}  // namespace clice::testing

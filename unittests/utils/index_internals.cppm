module;

#include "modules/prelude.h"

module clice:unittests.utils.index_internals;

import :index.project_index;
import :index.project_table;
import :index.shard;
import :vfs.ids;

namespace clice::testing {

/// Index state a test sets up or looks at by hand, past the public API.
struct IndexInternals {
    /// Install `shard` as `file`'s, as a merge or a load would.
    static void plant_shard(index::ProjectIndex& index, Fid file, index::Shard shard) {
        index.shards[file] = std::move(shard);
    }

    /// Give the index the project table generation a save would have
    /// reached.
    static void plant_generation(index::ProjectIndex& index, std::uint64_t generation) {
        index.generation = generation;
    }

    /// The generation the bound project table pins a TU's manifest at.
    static std::optional<std::uint64_t> pinned_generation(const index::ProjectIndex& index,
                                                          Fid tu) {
        auto unit = index.base_unit(tu);
        if(!unit) {
            return std::nullopt;
        }
        return index.base->columns->units[*unit].generation;
    }

    /// Whether the tables the manifests derive (contributions, absent
    /// lookups, importers) hold no entry, not even an emptied one.
    static bool derived_tables_empty(const index::ProjectIndex& index) {
        return index.contributions.empty() && index.absent_lookups.empty() &&
               index.importers.empty();
    }
};

}  // namespace clice::testing

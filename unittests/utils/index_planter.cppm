module;

#include "modules/prelude.h"

module clice:unittests.utils.index_planter;

import :index.project_index;
import :index.shard;
import :vfs.ids;

namespace clice::testing {

/// Index state a test sets up by hand, without the merge that would
/// produce it.
struct IndexPlanter {
    /// Install `shard` as `file`'s, as a merge or a load would.
    static void plant_shard(index::ProjectIndex& index, Fid file, index::Shard shard) {
        index.shards[file] = std::move(shard);
    }

    /// Give the index the project table generation a save would have
    /// reached.
    static void plant_generation(index::ProjectIndex& index, std::uint64_t generation) {
        index.generation = generation;
    }
};

}  // namespace clice::testing

#include "schema_generated.h"
#include "Index/ProjectIndex.h"
#include "Support/Ranges.h"

namespace clice::index {

void ProjectIndex::merge(this ProjectIndex& self, TUIndex& index) {
    auto& paths = index.graph.paths;
    llvm::SmallVector<std::uint32_t> file_ids_map;
    file_ids_map.resize_for_overwrite(paths.size());

    for(auto i = 0; i < paths.size(); i++) {
        file_ids_map[i] = self.path_pool.path_id(paths[i]);
    }

    for(auto& [symbol_id, symbol]: index.symbols) {
        auto& target_symbol = self.symbols[symbol_id];
        for(auto ref: symbol.reference_files) {
            target_symbol.reference_files.add(file_ids_map[ref]);
        }
    }
}

}  // namespace clice::index

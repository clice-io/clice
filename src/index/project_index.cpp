#include "index/project_index.h"

#include "index/serialization.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/STLExtras.h"

namespace clice::index {

llvm::SmallVector<std::uint32_t> ProjectIndex::merge(this ProjectIndex& self,
                                                     TUIndex& index,
                                                     clice::PathPool& pool) {
    auto& paths = index.graph.paths;
    llvm::SmallVector<std::uint32_t> file_ids_map;
    file_ids_map.resize_for_overwrite(paths.size());

    for(std::uint32_t i = 0; i < paths.size(); i++) {
        file_ids_map[i] = pool.intern(paths[i]);
    }

    for(auto& [symbol_id, symbol]: index.symbols) {
        if(symbol.scope != SymbolScope::External)
            continue;
        auto& target_symbol = self.symbols[symbol_id];
        if(target_symbol.name.empty()) {
            target_symbol.name = symbol.name;
            target_symbol.kind = symbol.kind;
        }
        for(auto ref: symbol.reference_files) {
            target_symbol.reference_files.add(file_ids_map[ref]);
        }
    }

    return file_ids_map;
}

void ProjectIndex::serialize(this ProjectIndex& self,
                             llvm::raw_ostream& os,
                             const clice::PathPool& pool,
                             llvm::ArrayRef<std::uint32_t> shards) {
    self.format_version = index_format_version;
    self.shards.assign(shards.begin(), shards.end());

    Bitmap referenced(shards.size(), shards.data());
    for(auto& symbol: llvm::make_second_range(self.symbols)) {
        referenced |= symbol.reference_files;
    }

    self.paths.clear();
    self.paths.reserve(referenced.cardinality());
    for(auto id: referenced) {
        self.paths.emplace_back(id, pool.resolve(id).str());
    }

    serialize_blob(self, os);
}

std::optional<ProjectIndex> ProjectIndex::from(llvm::StringRef data,
                                               clice::PathPool& pool,
                                               llvm::SmallVectorImpl<std::uint32_t>& shards) {
    std::optional<ProjectIndex> index{std::in_place};
    if(!deserialize_blob(data, *index) || index->format_version != index_format_version) {
        return std::nullopt;
    }

    // The blob's ids are the writing session's pool ids: intern its path
    // table and remap every decoded id into this session's pool. Ids the
    // table does not cover are dropped, not misresolved.
    llvm::DenseMap<std::uint32_t, std::uint32_t> remap;
    remap.reserve(index->paths.size());
    for(auto& [id, path]: index->paths) {
        // The writer only emits interned paths, which are never empty; an
        // empty entry marks a corrupt blob and must not become a real pool
        // entry.
        if(path.empty()) {
            return std::nullopt;
        }
        remap.try_emplace(id, pool.intern(path));
    }

    for(auto& symbol: llvm::make_second_range(index->symbols)) {
        Bitmap remapped;
        for(auto id: symbol.reference_files) {
            if(auto it = remap.find(id); it != remap.end()) {
                remapped.add(it->second);
            }
        }
        symbol.reference_files = std::move(remapped);
    }

    for(auto id: index->shards) {
        if(auto it = remap.find(id); it != remap.end()) {
            shards.push_back(it->second);
        }
    }

    // The table and manifest were only the wire form; the runtime state is
    // the pool and the caller's shard list.
    index->paths.clear();
    index->shards.clear();
    return index;
}

}  // namespace clice::index

#include "index/project_index.h"

#include "index/serialization.h"

#include "llvm/ADT/DenseMap.h"

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

void ProjectIndex::serialize(this const ProjectIndex& self,
                             llvm::raw_ostream& os,
                             const clice::PathPool& pool,
                             llvm::ArrayRef<std::uint32_t> shards) {
    // The persisted twin: same type, but bitmaps index the embedded compact
    // path table instead of the runtime pool.
    ProjectIndex persisted;

    // Compact path table: only ids the blob actually references are written,
    // in first-seen order. This is where garbage paths get collected — a
    // path the pool accumulated but nothing references never reaches disk.
    llvm::DenseMap<std::uint32_t, std::uint32_t> local_ids;
    auto to_local = [&](std::uint32_t pool_id) -> std::uint32_t {
        auto [it, inserted] = local_ids.try_emplace(pool_id, persisted.paths.size());
        if(inserted) {
            persisted.paths.emplace_back(pool.resolve(pool_id));
        }
        return it->second;
    };

    llvm::SmallVector<std::uint32_t> remapped;
    for(auto& [symbol_id, symbol]: self.symbols) {
        remapped.clear();
        for(auto ref: symbol.reference_files) {
            remapped.push_back(to_local(ref));
        }

        auto& persisted_symbol = persisted.symbols[symbol_id];
        persisted_symbol.name = symbol.name;
        persisted_symbol.kind = symbol.kind;
        persisted_symbol.scope = symbol.scope;
        persisted_symbol.reference_files = roaring::Roaring(remapped.size(), remapped.data());
    }

    persisted.shards.reserve(shards.size());
    for(auto shard: shards) {
        persisted.shards.push_back(to_local(shard));
    }

    write_flatbuffer(os, persisted);
}

std::optional<ProjectIndex> ProjectIndex::from(const void* data,
                                               std::size_t size,
                                               clice::PathPool& pool,
                                               llvm::SmallVectorImpl<std::uint32_t>& shards) {
    ProjectIndex loaded;
    if(!read_flatbuffer(data, size, loaded)) {
        return std::nullopt;
    }
    if(loaded.format_version != index_format_version) {
        return std::nullopt;
    }

    // Rebase the decoded blob onto the running pool: intern its compact path
    // table, rewrite every bitmap from blob-local ids to pool ids in place,
    // and hand the shard manifest out. Afterwards the object is in runtime
    // form, so the persisted-only fields are cleared.
    llvm::SmallVector<std::uint32_t> pool_ids;
    pool_ids.reserve(loaded.paths.size());
    for(auto& path: loaded.paths) {
        pool_ids.push_back(pool.intern(path));
    }

    auto to_pool = [&](std::uint32_t local) -> std::optional<std::uint32_t> {
        if(local >= pool_ids.size()) {
            return std::nullopt;
        }
        return pool_ids[local];
    };

    for(auto& [symbol_id, symbol]: loaded.symbols) {
        roaring::Roaring global_refs;
        for(auto local: symbol.reference_files) {
            if(auto id = to_pool(local)) {
                global_refs.add(*id);
            }
        }
        symbol.reference_files = std::move(global_refs);
    }

    for(auto local: loaded.shards) {
        if(auto id = to_pool(local)) {
            shards.push_back(*id);
        }
    }

    loaded.paths.clear();
    loaded.shards.clear();

    return loaded;
}

}  // namespace clice::index

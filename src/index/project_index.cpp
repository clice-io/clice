#include "index/project_index.h"

#include "index/serialization.h"

#include "llvm/ADT/DenseMap.h"

namespace clice::index {

namespace {

/// The persisted shape of the project blob: the symbol table with pool ids
/// remapped into a compact local path table (only ids the blob references
/// are written — garbage paths are collected here), plus the shard list as
/// local ids.
struct ProjectIndexRepr {
    std::uint32_t format_version = 0;
    std::vector<std::string> paths;
    SymbolTable symbols;
    std::vector<std::uint32_t> shards;
};

}  // namespace

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
    ProjectIndexRepr repr;
    repr.format_version = index_format_version;

    llvm::DenseMap<std::uint32_t, std::uint32_t> local_ids;
    auto to_local = [&](std::uint32_t pool_id) -> std::uint32_t {
        auto [it, inserted] = local_ids.try_emplace(pool_id, repr.paths.size());
        if(inserted) {
            repr.paths.emplace_back(pool.resolve(pool_id));
        }
        return it->second;
    };

    llvm::SmallVector<std::uint32_t> remapped;
    for(auto& [symbol_id, symbol]: self.symbols) {
        remapped.clear();
        for(auto ref: symbol.reference_files) {
            remapped.push_back(to_local(ref));
        }

        auto& target = repr.symbols[symbol_id];
        target.name = symbol.name;
        target.kind = symbol.kind;
        target.scope = symbol.scope;
        target.reference_files = Bitmap(remapped.size(), remapped.data());
    }

    for(auto shard: shards) {
        repr.shards.push_back(to_local(shard));
    }

    serialize_blob(repr, os);
}

std::optional<ProjectIndex> ProjectIndex::from(llvm::StringRef data,
                                               clice::PathPool& pool,
                                               llvm::SmallVectorImpl<std::uint32_t>& shards) {
    ProjectIndexRepr repr;
    if(!deserialize_blob(data, repr) || repr.format_version != index_format_version) {
        return std::nullopt;
    }

    // Intern the blob's compact path table into the running pool; every id
    // in the blob is an index into it.
    llvm::SmallVector<std::uint32_t> pool_ids;
    pool_ids.reserve(repr.paths.size());
    for(auto& path: repr.paths) {
        pool_ids.push_back(pool.intern(path));
    }

    auto to_pool = [&](std::uint32_t local) -> std::optional<std::uint32_t> {
        if(local >= pool_ids.size()) {
            return std::nullopt;
        }
        return pool_ids[local];
    };

    ProjectIndex loaded;
    for(auto& [symbol_id, symbol]: repr.symbols) {
        auto& target = loaded.symbols[symbol_id];
        target.name = std::move(symbol.name);
        target.kind = symbol.kind;
        target.scope = symbol.scope;
        for(auto local: symbol.reference_files) {
            if(auto id = to_pool(local)) {
                target.reference_files.add(*id);
            }
        }
    }

    for(auto local: repr.shards) {
        if(auto id = to_pool(local)) {
            shards.push_back(*id);
        }
    }

    return loaded;
}

}  // namespace clice::index

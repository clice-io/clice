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

namespace {

/// The persisted root of a ProjectIndex blob: the symbol table plus the
/// compact path table its bitmaps index into, the shard manifest, and the
/// format version. The runtime ProjectIndex only owns the symbols; the rest
/// is composed here at (de)serialization time.
struct PersistedProjectIndex {
    std::vector<std::string> paths;

    SymbolTable symbols;

    std::vector<std::uint32_t> shards;

    std::uint32_t format_version = index_format_version;
};

}  // namespace

void ProjectIndex::serialize(this const ProjectIndex& self,
                             llvm::raw_ostream& os,
                             const clice::PathPool& pool,
                             llvm::ArrayRef<std::uint32_t> shards) {
    PersistedProjectIndex persisted;

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

    auto encoded = kota::codec::fbs::to_flatbuffer(persisted);
    assert(encoded && "ProjectIndex flatbuffer serialization failed");
    if(!encoded) {
        return;
    }
    os.write(reinterpret_cast<const char*>(encoded->data()), encoded->size());
}

std::optional<ProjectIndex> ProjectIndex::from(const void* data,
                                               std::size_t size,
                                               clice::PathPool& pool,
                                               llvm::SmallVectorImpl<std::uint32_t>& shards) {
    std::span<const std::uint8_t> bytes(static_cast<const std::uint8_t*>(data), size);

    PersistedProjectIndex persisted;
    if(auto result = kota::codec::fbs::from_flatbuffer(bytes, persisted); !result) {
        return std::nullopt;
    }
    if(persisted.format_version != index_format_version) {
        return std::nullopt;
    }

    // Intern the blob's compact path table into the running pool; every id
    // in the blob is an index into it.
    llvm::SmallVector<std::uint32_t> pool_ids;
    pool_ids.reserve(persisted.paths.size());
    for(auto& path: persisted.paths) {
        pool_ids.push_back(pool.intern(path));
    }

    auto to_pool = [&](std::uint32_t local) -> std::optional<std::uint32_t> {
        if(local >= pool_ids.size()) {
            return std::nullopt;
        }
        return pool_ids[local];
    };

    ProjectIndex loaded;
    for(auto& [symbol_id, persisted_symbol]: persisted.symbols) {
        auto& symbol = loaded.symbols[symbol_id];
        symbol.name = std::move(persisted_symbol.name);
        symbol.kind = persisted_symbol.kind;
        symbol.scope = persisted_symbol.scope;
        for(auto local: persisted_symbol.reference_files) {
            if(auto id = to_pool(local)) {
                symbol.reference_files.add(*id);
            }
        }
    }

    for(auto local: persisted.shards) {
        if(auto id = to_pool(local)) {
            shards.push_back(*id);
        }
    }

    return loaded;
}

}  // namespace clice::index

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
    fbs::FlatBufferBuilder builder(1024);

    // Compact path table: only ids the blob actually references are written,
    // in first-seen order. This is where garbage paths get collected — a
    // path the pool accumulated but nothing references never reaches disk.
    llvm::DenseMap<std::uint32_t, std::uint32_t> local_ids;
    std::vector<llvm::StringRef> table;
    auto to_local = [&](std::uint32_t pool_id) -> std::uint32_t {
        auto [it, inserted] = local_ids.try_emplace(pool_id, table.size());
        if(inserted) {
            table.push_back(pool.resolve(pool_id));
        }
        return it->second;
    };

    llvm::SmallVector<char, 1024> buffer;
    llvm::SmallVector<std::uint32_t> remapped;

    auto symbols = transform(self.symbols, [&](auto&& value) {
        auto& [symbol_id, symbol] = value;

        remapped.clear();
        for(auto ref: symbol.reference_files) {
            remapped.push_back(to_local(ref));
        }
        roaring::Roaring local_refs(remapped.size(), remapped.data());

        buffer.clear();
        buffer.resize_for_overwrite(local_refs.getSizeInBytes(false));
        local_refs.write(buffer.data(), false);

        return binary::CreateSymbolEntry(builder,
                                         symbol_id,
                                         binary::CreateSymbol(builder,
                                                              CreateString(builder, symbol.name),
                                                              symbol.kind.value(),
                                                              CreateVector(builder, buffer),
                                                              static_cast<uint8_t>(symbol.scope)));
    });

    llvm::SmallVector<std::uint32_t> shard_locals;
    shard_locals.reserve(shards.size());
    for(auto shard: shards) {
        shard_locals.push_back(to_local(shard));
    }

    auto paths =
        transform(table, [&](llvm::StringRef path) { return CreateString(builder, path); });

    auto project_index =
        binary::CreateProjectIndex(builder,
                                   CreateVector(builder, paths),
                                   CreateVector(builder, symbols),
                                   builder.CreateVector(shard_locals.data(), shard_locals.size()),
                                   index_format_version);

    builder.Finish(project_index);
    os.write(safe_cast<const char>(builder.GetBufferPointer()), builder.GetSize());
}

std::optional<ProjectIndex> ProjectIndex::from(const void* data,
                                               std::size_t size,
                                               clice::PathPool& pool,
                                               llvm::SmallVectorImpl<std::uint32_t>& shards) {
    fbs::Verifier verifier(static_cast<const std::uint8_t*>(data), size);
    if(!verifier.VerifyBuffer<binary::ProjectIndex>()) {
        return std::nullopt;
    }

    auto root = fbs::GetRoot<binary::ProjectIndex>(data);
    if(root->format_version() != index_format_version) {
        return std::nullopt;
    }

    ProjectIndex loaded;

    // Intern the blob's compact path table into the running pool; every id
    // in the blob is an index into it.
    llvm::SmallVector<std::uint32_t> pool_ids;
    if(root->paths()) {
        pool_ids.reserve(root->paths()->size());
        for(auto path: *root->paths()) {
            pool_ids.push_back(pool.intern(path->string_view()));
        }
    }

    auto to_pool = [&](std::uint32_t local) -> std::optional<std::uint32_t> {
        if(local >= pool_ids.size()) {
            return std::nullopt;
        }
        return pool_ids[local];
    };

    if(root->symbols()) {
        for(auto entry: *root->symbols()) {
            auto* fb_symbol = entry->symbol();
            if(!fb_symbol) {
                continue;
            }
            auto& symbol = loaded.symbols[entry->symbol_id()];
            if(auto* name = fb_symbol->name()) {
                symbol.name = name->str();
            }
            symbol.kind = SymbolKind(static_cast<std::uint8_t>(fb_symbol->kind()));
            symbol.scope = static_cast<index::SymbolScope>(fb_symbol->scope());
            for(auto local: read_bitmap(fb_symbol->refs())) {
                if(auto id = to_pool(local)) {
                    symbol.reference_files.add(*id);
                }
            }
        }
    }

    if(root->shards()) {
        for(auto local: *root->shards()) {
            if(auto id = to_pool(local)) {
                shards.push_back(*id);
            }
        }
    }

    return loaded;
}

}  // namespace clice::index

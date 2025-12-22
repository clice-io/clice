#include "Index/ProjectIndex.h"

#include "Serialization.h"
#include "Support/Ranges.h"

namespace clice::index {

llvm::SmallVector<std::uint32_t> ProjectIndex::merge(this ProjectIndex& self, TUIndex& index) {
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

    return file_ids_map;
}

void ProjectIndex::serialize(this ProjectIndex& self, llvm::raw_ostream& os) {
    fbs::FlatBufferBuilder builder(1024);

    llvm::SmallVector<char, 1024> buffer;

    auto i = 0;
    auto paths = transform(self.path_pool.paths, [&](llvm::StringRef path) {
        auto entry =
            binary::CreatePathEntry(builder, CreateString(builder, self.path_pool.paths[i]), i);
        i += 1;
        return entry;
    });

    auto indices = transform(self.indices, [&](auto&& value) {
        auto&& [source, index] = value;
        return binary::PathMapEntry(source, index);
    });

    auto symbols = transform(self.symbols, [&](auto&& value) {
        auto& [symbol_id, symbol] = value;

        buffer.clear();
        buffer.resize_for_overwrite(symbol.reference_files.getSizeInBytes(false));
        symbol.reference_files.write(buffer.data(), false);

        return binary::CreateSymbolEntry(
            builder,
            symbol_id,
            binary::CreateSymbol(builder, symbol.kind.value(), CreateVector(builder, buffer)));
    });

    auto project_index =
        binary::CreateProjectIndex(builder,
                                   CreateVector(builder, paths),
                                   CreateStructVector<binary::PathMapEntry>(builder, indices),
                                   CreateVector(builder, symbols));

    builder.Finish(project_index);
    os.write(safe_cast<const char>(builder.GetBufferPointer()), builder.GetSize());
}

ProjectIndex ProjectIndex::from(const void* data) {
    auto root = fbs::GetRoot<binary::ProjectIndex>(data);

    ProjectIndex index;

    auto& pool = index.path_pool;
    pool.paths.resize(root->paths()->size());
    for(auto entry: *root->paths()) {
        auto k = pool.save(entry->path()->string_view());
        pool.paths[entry->id()] = k;
        pool.cache.try_emplace(k, entry->id());
    }

    for(auto entry: *root->indices()) {
        index.indices.try_emplace(entry->source(), entry->index());
    }

    for(auto entry: *root->symbols()) {
        auto& symbol = index.symbols[entry->symbol_id()];
        symbol.kind = SymbolKind(entry->symbol()->kind());
        symbol.reference_files = read_bitmap(entry->symbol()->refs());
    }

    return index;
}

}  // namespace clice::index

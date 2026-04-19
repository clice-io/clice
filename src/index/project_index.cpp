#include "index/project_index.h"

#include <cassert>
#include <cstdint>
#include <span>

#include "index/kotatsu_adapters.h"  // type_adapter specializations

#include "kota/codec/flatbuffers/deserializer.h"
#include "kota/codec/flatbuffers/serializer.h"

namespace clice::index {

namespace {

namespace kfb = kota::codec::flatbuffers;

}  // namespace

llvm::SmallVector<std::uint32_t> ProjectIndex::merge(this ProjectIndex& self, TUIndex& index) {
    auto& paths = index.graph.paths;
    llvm::SmallVector<std::uint32_t> file_ids_map;
    file_ids_map.resize_for_overwrite(paths.size());

    for(std::uint32_t i = 0; i < paths.size(); i++) {
        file_ids_map[i] = self.path_pool.path_id(paths[i]);
    }

    for(auto& [symbol_id, symbol]: index.symbols) {
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

void ProjectIndex::serialize(this ProjectIndex& self, llvm::raw_ostream& os) {
    auto bytes = kfb::to_flatbuffer(self);
    assert(bytes && "ProjectIndex flatbuffer serialization failed");
    os.write(reinterpret_cast<const char*>(bytes->data()), bytes->size());
}

ProjectIndex ProjectIndex::from(const void* data, std::size_t size) {
    ProjectIndex index;
    if(data == nullptr || size == 0) {
        return index;
    }

    std::span<const std::uint8_t> bytes(static_cast<const std::uint8_t*>(data), size);
    auto result = kfb::from_flatbuffer(bytes, index);
    if(!result) {
        return ProjectIndex();
    }
    return index;
}

}  // namespace clice::index

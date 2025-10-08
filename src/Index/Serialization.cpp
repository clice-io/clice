#include "schema_generated.h"
#include "Index/MergedIndex.h"
#include "Index/ProjectIndex.h"
#include "Support/Ranges.h"

namespace clice::index {

namespace fbs = flatbuffers;

namespace {

template <typename T>
using Offsets = llvm::SmallVector<fbs::Offset<T>, 0>;

template <typename U, typename V>
const U* safe_cast(const V* v) {
    static_assert(sizeof(U) == sizeof(V));
    assert((void(std::bit_cast<U>(V{})), true));
    return reinterpret_cast<const U*>(v);
}

auto CreateString(fbs::FlatBufferBuilder& builder, llvm::StringRef string) {
    return builder.CreateString(string.data(), string.size());
}

template <sequence_range Range>
auto CreateVector(fbs::FlatBufferBuilder& builder, const Range& range) {
    return builder.CreateVector(range.data(), range.size());
}

auto CreateVector(fbs::FlatBufferBuilder& builder, const llvm::SmallVector<char, 1024>& range) {
    return builder.CreateVector(reinterpret_cast<const std::uint8_t*>(range.data()), range.size());
}

template <typename U, sequence_range Range>
auto CreateStructVector(fbs::FlatBufferBuilder& builder, const Range& range) {
    using V = ranges::range_value_t<Range>;
    return builder.CreateVectorOfStructs(safe_cast<U>(range.data()), range.size());
}

template <typename Range, typename Functor>
auto transform(const Range& range, const Functor& functor) {
    using V = ranges::range_value_t<Range>;
    using R = std::invoke_result_t<Functor, V>;

    llvm::SmallVector<R, 0> result;
    result.resize_for_overwrite(ranges::size(range));

    auto i = 0;
    for(auto&& v: range) {
        result[i] = functor(v);
        i += 1;
    }
    return result;
}

Bitmap read_bitmap(const fbs::Vector<uint8_t>* buffer) {
    return Bitmap::read(reinterpret_cast<const char*>(buffer->data()), false);
}

}  // namespace

void MergedIndex::serialize(this MergedIndex& self, llvm::raw_ostream& out) {
    fbs::FlatBufferBuilder builder(1024);

    llvm::SmallVector<char, 1024> buffer;

    auto canonical_cache = transform(self.canonical_cache, [&](auto&& value) {
        auto&& [hash, canonical_id] = value;
        return binary::CreateCacheEntry(builder, CreateString(builder, hash), canonical_id);
    });

    auto header_contexts = transform(self.contexts, [&](auto&& value) {
        auto& [path, contexts] = value;
        return binary::CreateHeaderContextsEntry(
            builder,
            CreateString(builder, path),
            binary::CreateHeaderContexts(
                builder,
                contexts.version,
                CreateStructVector<binary::Context>(builder, contexts.includes)));
    });

    auto occurrences = transform(self.occurrences, [&](auto&& value) {
        auto&& [occurrence, bitmap] = value;
        buffer.clear();
        buffer.resize_for_overwrite(bitmap.getSizeInBytes(false));
        bitmap.write(buffer.data(), false);
        return binary::CreateOccurrenceEntry(builder,
                                             safe_cast<binary::Occurrence>(&occurrence),
                                             CreateVector(builder, buffer));
    });

    auto relations = transform(self.relations, [&](auto&& value) {
        auto&& [symbold_id, symbol_relations] = value;
        auto relations = transform(symbol_relations, [&](auto&& value) {
            auto&& [relation, bitmap] = value;
            buffer.clear();
            buffer.resize_for_overwrite(bitmap.getSizeInBytes(false));
            bitmap.write(buffer.data(), false);
            return binary::CreateRelationEntry(builder,
                                               safe_cast<binary::Relation>(&relation),
                                               CreateVector(builder, buffer));
        });
        return binary::CreateSymbolRelationsEntry(builder,
                                                  symbold_id,
                                                  CreateVector(builder, relations));
    });

    auto merged_index = binary::CreateMergedIndex(builder,
                                                  self.max_canonical_id,
                                                  CreateVector(builder, canonical_cache),
                                                  CreateVector(builder, header_contexts),
                                                  CreateVector(builder, occurrences),
                                                  CreateVector(builder, relations));
    builder.Finish(merged_index);

    out.write(safe_cast<char>(builder.GetBufferPointer()), builder.GetSize());
}

MergedIndex MergedIndexView::deserialize() {
    auto root = fbs::GetRoot<binary::MergedIndex>(data);

    MergedIndex index;
    index.max_canonical_id = root->max_canonical_id();

    for(auto entry: *root->canonical_cache()) {
        index.canonical_cache.try_emplace(entry->sha256()->string_view(), entry->canonical_id());
    }

    index.canonical_ref_counts.resize(index.max_canonical_id, 0);

    HeaderContexts contexts;
    for(auto entry: *root->contexts()) {
        auto path = entry->path()->string_view();
        contexts.version = entry->contexts()->version();
        for(auto include: *entry->contexts()->includes()) {
            index.canonical_ref_counts[include->canonical_id()] += 1;
            contexts.includes.emplace_back(include->include_(), include->canonical_id());
        }
        index.contexts.try_emplace(path, std::move(contexts));
    }

    for(auto entry: *root->occurrences()) {
        index.occurrences.try_emplace(*safe_cast<Occurrence>(entry->occurrence()),
                                      read_bitmap(entry->context()));
    }

    for(auto entry: *root->relations()) {
        auto& relations = index.relations[entry->symbol()];
        for(auto relation_entry: *entry->relations()) {
            relations.try_emplace(*safe_cast<Relation>(relation_entry->relation()),
                                  read_bitmap(relation_entry->context()));
        }
    }

    return index;
}

void ProjectIndex::serialize(this ProjectIndex& self, llvm::raw_ostream& os) {
    fbs::FlatBufferBuilder builder(1024);

    llvm::SmallVector<char, 1024> buffer;

    auto i = 0;
    auto paths = transform(self.path_pool.paths, [&](llvm::StringRef path) {
        auto enrty =
            binary::CreatePathEntry(builder, CreateString(builder, self.path_pool.paths[i]), i);
        i += 1;
        return enrty;
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

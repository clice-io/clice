#include "schema_generated.h"
#include "Index/MergedIndex.h"
#include "llvm/Support/SHA256.h"
#include "llvm/Support/raw_os_ostream.h"

namespace clice::index {

namespace {

auto sha256_hash(FileIndex& index) {
    llvm::SHA256 hasher;

    using u8 = std::uint8_t;

    if(!index.occurrences.empty()) {
        static_assert(sizeof(Occurrence) == sizeof(Range) + sizeof(SymbolHash));
        static_assert(sizeof(Occurrence) % 8 == 0);
        auto data = reinterpret_cast<u8*>(index.occurrences.data());
        auto size = index.occurrences.size() * sizeof(Occurrence);
        hasher.update(llvm::ArrayRef(data, size));
    }

    for(auto& [symbol_id, relations]: index.relations) {
        hasher.update(std::bit_cast<std::array<u8, sizeof(symbol_id)>>(symbol_id));
        static_assert(sizeof(Relation) ==
                      sizeof(RelationKind) + 4 + sizeof(Range) + sizeof(SymbolHash));
        static_assert(sizeof(Relation) % 8 == 0);

        if(!relations.empty()) {
            auto data = reinterpret_cast<u8*>(relations.data());
            auto size = relations.size() * sizeof(Relation);
            hasher.update(llvm::ArrayRef(data, size));
        }
    }

    return hasher.final();
}

}  // namespace

void MergedIndex::remove(llvm::StringRef path) {
    auto& includes = contexts[path].includes;

    for(auto& [_, canonical_id]: includes) {
        auto& ref_counts = canonical_ref_counts[canonical_id];
        ref_counts -= 1;

        if(ref_counts == 0) {
            removed.add(canonical_id);
        }
    }

    includes.clear();
}

void MergedIndex::merge(llvm::StringRef path, std::uint32_t include, FileIndex& index) {
    auto& context = contexts[path];

    auto hash = sha256_hash(index);
    auto hash_key = llvm::StringRef(reinterpret_cast<char*>(hash.data()), hash.size());
    auto [it, success] = canonical_cache.try_emplace(hash_key, max_canonical_id);

    auto canonical_id = it->second;
    context.includes.emplace_back(include, canonical_id);

    if(!success) {
        canonical_ref_counts[canonical_id] += 1;
        removed.remove(canonical_id);
        return;
    }

    for(auto& occurrence: index.occurrences) {
        this->occurrences[occurrence].add(canonical_id);
    }

    for(auto& [symbol_id, relations]: index.relations) {
        auto& target = this->relations[symbol_id];
        for(auto& relation: relations) {
            target[relation].add(canonical_id);
        }
    }

    canonical_ref_counts.emplace_back(1);
    max_canonical_id += 1;
}

void MergedIndex::serialize(this MergedIndex& self, llvm::raw_ostream& out) {
    namespace fbs = flatbuffers;
    fbs::FlatBufferBuilder builder(1024);

    std::vector<fbs::Offset<binary::CacheEntry>> canonical_cache;
    canonical_cache.reserve(self.canonical_cache.size());
    for(auto& [hash, canonical_id]: self.canonical_cache) {
        canonical_cache.emplace_back(
            binary::CreateCacheEntry(builder,
                                     builder.CreateString(hash.data(), hash.size()),
                                     canonical_id));
    };

    std::vector<fbs::Offset<binary::HeaderContextsEntry>> header_contexts;
    header_contexts.reserve(self.contexts.size());
    for(auto& [path, contexts]: self.contexts) {
        header_contexts.emplace_back(binary::CreateHeaderContextsEntry(
            builder,
            builder.CreateString(path.data(), path.size()),
            binary::CreateHeaderContexts(
                builder,
                contexts.version,
                builder.CreateVectorOfStructs(
                    reinterpret_cast<binary::Context*>(contexts.includes.data()),
                    contexts.includes.size()))));
    };

    llvm::SmallVector<char, 256> buffer;

    std::vector<fbs::Offset<binary::OccurrenceEntry>> occurrences;
    occurrences.reserve(self.occurrences.size());
    for(auto& [occurrence, bitmap]: self.occurrences) {
        buffer.resize_for_overwrite(bitmap.getSizeInBytes(false));
        bitmap.write(buffer.data(), false);
        occurrences.emplace_back(binary::CreateOccurrenceEntry(
            builder,
            reinterpret_cast<binary::Occurrence*>(&occurrence),
            builder.CreateVector(reinterpret_cast<const uint8_t*>(buffer.data()), buffer.size())));
        buffer.clear();
    }

    std::vector<fbs::Offset<binary::SymbolRelationsEntry>> relations;
    relations.reserve(self.relations.size());
    for(auto& [symbold_id, symbol_relations]: self.relations) {
        std::vector<fbs::Offset<binary::RelationEntry>> entries;
        entries.reserve(symbol_relations.size());
        for(auto& [relation, bitmap]: symbol_relations) {
            buffer.resize_for_overwrite(bitmap.getSizeInBytes(false));
            bitmap.write(buffer.data(), false);
            entries.emplace_back(binary::CreateRelationEntry(
                builder,
                reinterpret_cast<binary::Relation*>(&relation),
                builder.CreateVector(reinterpret_cast<const uint8_t*>(buffer.data()),
                                     buffer.size())));
            buffer.clear();
        }

        relations.emplace_back(
            binary::CreateSymbolRelationsEntryDirect(builder, symbold_id, &entries));
    }

    auto merged_index = binary::CreateMergedIndexDirect(builder,
                                                        self.max_canonical_id,
                                                        &canonical_cache,
                                                        &header_contexts,
                                                        &occurrences,
                                                        &relations);
    builder.Finish(merged_index);

    out.write(reinterpret_cast<char*>(builder.GetBufferPointer()), builder.GetSize());
}

MergedIndex MergedIndexView::deserialize() {
    namespace fbs = flatbuffers;
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
        index.occurrences.try_emplace(
            *reinterpret_cast<const Occurrence*>(entry->occurrence()),
            Bitmap::read(reinterpret_cast<const char*>(entry->context()->data()), false));
    }

    for(auto entry: *root->relations()) {
        auto& relations = index.relations[entry->symbol()];
        for(auto relation_entry: *entry->relations()) {
            relations.try_emplace(
                *reinterpret_cast<const Relation*>(relation_entry->relation()),
                Bitmap::read(reinterpret_cast<const char*>(relation_entry->context()->data()),
                             false));
        }
    }

    return index;
}

}  // namespace clice::index

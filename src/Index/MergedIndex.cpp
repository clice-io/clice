#include "Serialization.h"
#include "Support/Compare.h"
#include "Support/FileSystem.h"
#include "Index/MergedIndex.h"
#include "llvm/Support/raw_os_ostream.h"

namespace llvm {

template <typename... Ts>
unsigned dense_hash(const Ts&... ts) {
    return llvm::DenseMapInfo<std::tuple<Ts...>>::getHashValue(std::tuple{ts...});
}

template <>
struct DenseMapInfo<clice::index::Occurrence> {
    using R = clice::LocalSourceRange;
    using V = clice::index::Occurrence;

    inline static V getEmptyKey() {
        return V(R(-1, 0), 0);
    }

    inline static V getTombstoneKey() {
        return V(R(-2, 0), 0);
    }

    static auto getHashValue(const V& v) {
        return dense_hash(v.range.begin, v.range.end, v.target);
    }

    static bool isEqual(const V& lhs, const V& rhs) {
        return lhs.range == rhs.range && lhs.target == rhs.target;
    }
};

template <>
struct DenseMapInfo<clice::index::Relation> {
    using R = clice::index::Relation;

    inline static R getEmptyKey() {
        return R{
            .kind = clice::RelationKind(),
            .range = clice::LocalSourceRange(-1, 0),
            .target_symbol = 0,
        };
    }

    inline static R getTombstoneKey() {
        return R{
            .kind = clice::RelationKind(),
            .range = clice::LocalSourceRange(-2, 0),
            .target_symbol = 0,
        };
    }

    /// Contextual doen't take part in hashing and equality.
    static auto getHashValue(const R& relation) {
        return dense_hash(relation.kind.value(),
                          relation.range.begin,
                          relation.range.end,
                          relation.target_symbol);
    }

    static bool isEqual(const R& lhs, const R& rhs) {
        return lhs.kind == rhs.kind && lhs.range == rhs.range &&
               lhs.target_symbol == rhs.target_symbol;
    }
};
}  // namespace llvm

namespace clice::index {

using HeaderContexts = llvm::DenseMap<std::uint32_t, HeaderContext>;

struct MergedIndex::Impl {
    /// FIXME: The content of this file.
    std::string content;

    /// A map between source file path and its header contexts.
    HeaderContexts header_contexts;

    /// For each merged index, we will give it a canonical id.
    /// The max canonical id.
    std::uint32_t max_canonical_id = 0;

    /// We use the value of SHA256 to judge whether two indices are same.
    /// Index with same content will be given same canonical id.
    llvm::StringMap<std::uint32_t> canonical_cache;

    /// The reference count of each canonical id.
    std::vector<std::uint32_t> canonical_ref_counts;

    /// The canonical id set of removed index.
    roaring::Roaring removed;

    /// All merged symbol occurrences.
    llvm::DenseMap<Occurrence, roaring::Roaring> occurrences;

    /// All merged symbol relations.
    llvm::DenseMap<SymbolHash, llvm::DenseMap<Relation, roaring::Roaring>> relations;

    /// Sorted occurrences cache for fast lookup.
    std::vector<Occurrence> occurrences_cache;

    friend bool operator== (const Impl&, const Impl&) = default;
};

void MergedIndex::load_in_memory(this Self& self) {
    if(self.impl) {
        return;
    }

    self.impl = new MergedIndex::Impl();
    if(!self.buffer) {
        return;
    }

    auto& index = *self.impl;
    auto root = fbs::GetRoot<binary::MergedIndex>(self.buffer->getBuffer().data());

    index.max_canonical_id = root->max_canonical_id();

    for(auto entry: *root->canonical_cache()) {
        index.canonical_cache.try_emplace(entry->sha256()->string_view(), entry->canonical_id());
    }

    index.canonical_ref_counts.resize(index.max_canonical_id, 0);

    HeaderContext contexts;
    for(auto entry: *root->contexts()) {
        auto path = entry->path();
        contexts.version = entry->contexts()->version();
        for(auto include: *entry->contexts()->includes()) {
            index.canonical_ref_counts[include->canonical_id()] += 1;
            contexts.includes.emplace_back(include->include_(), include->canonical_id());
        }
        index.header_contexts.try_emplace(path, std::move(contexts));
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
}

MergedIndex MergedIndex::load(llvm::StringRef path) {
    auto buffer = llvm::MemoryBuffer::getFile(path);
    if(!buffer) {
        return MergedIndex();
    } else {
        return MergedIndex(std::move(*buffer), nullptr);
    }
}

MergedIndex::~MergedIndex() {
    delete impl;
}

void MergedIndex::serialize(this const Self& self, llvm::raw_ostream& out) {
    auto index = self.impl;

    fbs::FlatBufferBuilder builder(1024);

    llvm::SmallVector<char, 1024> buffer;

    auto canonical_cache = transform(index->canonical_cache, [&](auto&& value) {
        auto&& [hash, canonical_id] = value;
        return binary::CreateCacheEntry(builder, CreateString(builder, hash), canonical_id);
    });

    auto header_contexts = transform(index->header_contexts, [&](auto&& value) {
        auto& [path_id, contexts] = value;
        return binary::CreateHeaderContextsEntry(
            builder,
            path_id,
            binary::CreateHeaderContexts(
                builder,
                contexts.version,
                CreateStructVector<binary::Context>(builder, contexts.includes)));
    });

    auto occurrences = transform(index->occurrences, [&](auto&& value) {
        auto&& [occurrence, bitmap] = value;
        buffer.clear();
        buffer.resize_for_overwrite(bitmap.getSizeInBytes(false));
        bitmap.write(buffer.data(), false);
        return binary::CreateOccurrenceEntry(builder,
                                             safe_cast<binary::Occurrence>(&occurrence),
                                             CreateVector(builder, buffer));
    });

    auto relations = transform(index->relations, [&](auto&& value) {
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
                                                  index->max_canonical_id,
                                                  CreateVector(builder, canonical_cache),
                                                  CreateVector(builder, header_contexts),
                                                  CreateVector(builder, occurrences),
                                                  CreateVector(builder, relations));
    builder.Finish(merged_index);

    out.write(safe_cast<char>(builder.GetBufferPointer()), builder.GetSize());
}

void MergedIndex::lookup(this const Self& self,
                         std::uint32_t offset,
                         llvm::function_ref<bool(const Occurrence&)> callback) {
    if(self.impl) {
        auto& index = *self.impl;
        auto& occurrences = index.occurrences_cache;
        if(occurrences.empty()) {
            for(auto& [o, _]: index.occurrences) {
                occurrences.emplace_back(o);
            }
            ranges::sort(occurrences, refl::less);
        }

        auto it = ranges::lower_bound(occurrences, offset, {}, [](index::Occurrence& o) {
            return o.range.end;
        });

        while(it != occurrences.end()) {
            if(it->range.contains(offset)) {
                if(!callback(*it)) {
                    break;
                }

                it++;
                continue;
            }

            break;
        }
    } else if(self.buffer) {
        auto index = fbs::GetRoot<binary::MergedIndex>(self.buffer->getBuffer().data());
        auto& occurrences = *index->occurrences();

        auto it = ranges::lower_bound(occurrences, offset, {}, [](auto o) {
            return o->occurrence()->range().end();
        });

        while(it != occurrences.end()) {
            auto o = safe_cast<Occurrence>(it->occurrence());
            if(o->range.contains(offset)) {
                if(!callback(*o)) {
                    break;
                }

                it++;
                continue;
            }

            break;
        }
    }
}

void MergedIndex::lookup(this const Self& self,
                         SymbolHash symbol,
                         RelationKind kind,
                         llvm::function_ref<bool(const Relation&)> callback) {

    if(self.impl) {
        auto& relations = self.impl->relations[symbol];
        for(auto& [relation, _]: relations) {
            if(!callback(relation)) {
                break;
            }
        }
    } else if(self.buffer) {
        auto index = fbs::GetRoot<binary::MergedIndex>(self.buffer->getBuffer().data());
        auto& entries = *index->relations();

        auto it = ranges::lower_bound(entries, symbol, {}, [](auto e) { return e->symbol(); });
        if(it == entries.end() || it->symbol() != symbol) {
            return;
        }

        for(auto entry: *it->relations()) {
            auto r = safe_cast<Relation>(entry->relation());
            if(r->kind & kind) {
                if(!callback(*r)) {
                    break;
                }
            }
        }
    }
}

void MergedIndex::remove(this Self& self, std::uint32_t path_id) {
    self.load_in_memory();
    auto& index = *self.impl;

    auto& includes = index.header_contexts[path_id].includes;

    for(auto& [_, canonical_id]: includes) {
        auto& ref_counts = index.canonical_ref_counts[canonical_id];
        ref_counts -= 1;

        if(ref_counts == 0) {
            index.removed.add(canonical_id);
        }
    }

    includes.clear();
}

void MergedIndex::merge(this Self& self,
                        std::uint32_t path_id,
                        std::vector<IncludeLocation> includes) {
    self.load_in_memory();
}

void MergedIndex::merge(this Self& self,
                        std::uint32_t path_id,
                        std::uint32_t include_id,
                        FileIndex& index) {
    self.load_in_memory();
    auto& impl = *self.impl;

    auto& context = impl.header_contexts[path_id];

    auto hash = index.hash();
    auto hash_key = llvm::StringRef(reinterpret_cast<char*>(hash.data()), hash.size());
    auto [it, success] = impl.canonical_cache.try_emplace(hash_key, impl.max_canonical_id);

    auto canonical_id = it->second;
    context.includes.emplace_back(include_id, canonical_id);

    if(!success) {
        impl.canonical_ref_counts[canonical_id] += 1;
        impl.removed.remove(canonical_id);
        return;
    }

    for(auto& occurrence: index.occurrences) {
        impl.occurrences[occurrence].add(canonical_id);
    }

    for(auto& [symbol_id, relations]: index.relations) {
        auto& target = impl.relations[symbol_id];
        for(auto& relation: relations) {
            target[relation].add(canonical_id);
        }
    }

    impl.canonical_ref_counts.emplace_back(1);
    impl.max_canonical_id += 1;
}

bool operator== (MergedIndex& lhs, MergedIndex& rhs) {
    lhs.load_in_memory();
    rhs.load_in_memory();
    return *lhs.impl == *rhs.impl;
}

}  // namespace clice::index

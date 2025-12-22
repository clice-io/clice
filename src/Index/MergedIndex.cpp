#include "Index/MergedIndex.h"

#include "Serialization.h"
#include "Support/Compare.h"
#include "Support/FileSystem.h"

#include "llvm/ADT/DenseSet.h"
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

    /// Contextual doesn’t take part in hashing and equality.
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

struct IncludeContext {
    std::uint32_t include_id;

    std::uint32_t canonical_id;

    friend bool operator==(const IncludeContext&, const IncludeContext&) = default;
};

struct HeaderContext {
    std::uint32_t version = 0;

    llvm::SmallVector<IncludeContext> includes;

    friend bool operator==(const HeaderContext&, const HeaderContext&) = default;
};

struct CompilationContext {
    std::uint32_t version = 0;

    std::uint32_t canonical_id = 0;

    std::uint64_t build_at;

    std::vector<IncludeLocation> include_locations;

    friend bool operator==(const CompilationContext&, const CompilationContext&) = default;
};

struct MergedIndex::Impl {
    /// The content of corresponding source file.
    std::string content;

    /// If this file is included by other source file, then it has header contexts.
    /// The key represents the source file id, value represents the context in the
    /// source file.
    llvm::SmallDenseMap<std::uint32_t, HeaderContext, 2> header_contexts;

    /// If this file is compiled as source file, then it has compilation contexts.
    /// The key represents the compilation command id. File with compilation content
    /// could provide header contexts for other files.
    llvm::SmallDenseMap<std::uint32_t, CompilationContext, 1> compilation_contexts;

    /// We use the value of SHA256 to judge whether two indices are same.
    /// The same indices will be given same canonical id.
    llvm::StringMap<std::uint32_t> canonical_cache;

    /// The max canonical id we have allocated.
    std::uint32_t max_canonical_id = 0;

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

    void merge(this Impl& self, std::uint32_t path_id, FileIndex& index, auto&& add_context) {
        auto hash = index.hash();
        auto hash_key = llvm::StringRef(reinterpret_cast<char*>(hash.data()), hash.size());
        auto [it, success] = self.canonical_cache.try_emplace(hash_key, self.max_canonical_id);

        auto canonical_id = it->second;
        add_context(self, canonical_id);

        if(!success) {
            self.canonical_ref_counts[canonical_id] += 1;
            self.removed.remove(canonical_id);
            return;
        }

        for(auto& occurrence: index.occurrences) {
            self.occurrences[occurrence].add(canonical_id);
        }

        for(auto& [symbol_id, relations]: index.relations) {
            auto& target = self.relations[symbol_id];
            for(auto& relation: relations) {
                target[relation].add(canonical_id);
            }
        }

        self.canonical_ref_counts.emplace_back(1);
        self.max_canonical_id += 1;
    }

    friend bool operator==(const Impl&, const Impl&) = default;
};

MergedIndex::MergedIndex(std::unique_ptr<llvm::MemoryBuffer> buffer, std::unique_ptr<Impl> impl) :
    buffer(std::move(buffer)), impl(std::move(impl)) {}

MergedIndex::MergedIndex() = default;

MergedIndex::MergedIndex(llvm::StringRef data) :
    MergedIndex(llvm::MemoryBuffer::getMemBuffer(data, "", false), nullptr) {}

MergedIndex::MergedIndex(MergedIndex&& other) = default;

MergedIndex& MergedIndex::operator=(MergedIndex&& other) = default;

MergedIndex::~MergedIndex() = default;

void MergedIndex::load_in_memory(this Self& self) {
    if(self.impl) {
        return;
    }

    self.impl = std::make_unique<MergedIndex::Impl>();
    if(!self.buffer) {
        return;
    }

    auto& index = *self.impl;
    auto root = fbs::GetRoot<binary::MergedIndex>(self.buffer->getBufferStart());

    index.max_canonical_id = root->max_canonical_id();

    for(auto entry: *root->canonical_cache()) {
        index.canonical_cache.try_emplace(entry->sha256()->string_view(), entry->canonical_id());
    }

    index.canonical_ref_counts.resize(index.max_canonical_id, 0);

    for(auto entry: *root->header_contexts()) {
        HeaderContext context;
        auto path = entry->path_id();
        context.version = entry->version();
        for(auto include: *entry->includes()) {
            index.canonical_ref_counts[include->canonical_id()] += 1;
            context.includes.emplace_back(*safe_cast<IncludeContext>(include));
        }
        index.header_contexts.try_emplace(path, std::move(context));
    }

    for(auto entry: *root->compilation_contexts()) {
        CompilationContext context;
        auto path = entry->path_id();
        context.version = entry->version();
        context.canonical_id = entry->canonical_id();
        context.build_at = entry->build_at();
        for(auto include: *entry->include_locations()) {
            context.include_locations.emplace_back(*safe_cast<IncludeLocation>(include));
        }
        index.compilation_contexts.try_emplace(path, std::move(context));
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

    self.buffer.reset();
}

MergedIndex MergedIndex::load(llvm::StringRef path) {
    auto buffer = llvm::MemoryBuffer::getFile(path);
    if(!buffer) {
        return MergedIndex();
    } else {
        return MergedIndex(std::move(*buffer), nullptr);
    }
}

void MergedIndex::serialize(this const Self& self, llvm::raw_ostream& out) {
    if(self.buffer) {
        out.write(self.buffer->getBufferStart(), self.buffer->getBufferSize());
        return;
    }

    if(!self.impl) {
        return;
    }

    auto& index = self.impl;

    fbs::FlatBufferBuilder builder(1024);

    llvm::SmallVector<char, 1024> buffer;

    auto canonical_cache = transform(index->canonical_cache, [&](auto&& value) {
        auto&& [hash, canonical_id] = value;
        return binary::CreateCacheEntry(builder, CreateString(builder, hash), canonical_id);
    });

    auto header_contexts = transform(index->header_contexts, [&](auto&& value) {
        auto& [path_id, context] = value;
        return binary::CreateHeaderContextEntry(
            builder,
            path_id,
            context.version,
            CreateStructVector<binary::IncludeContext>(builder, context.includes));
    });

    auto compilation_contexts = transform(index->compilation_contexts, [&](auto&& value) {
        auto& [path_id, context] = value;
        return binary::CreateCompilationContextEntry(
            builder,
            path_id,
            context.version,
            context.canonical_id,
            context.build_at,
            CreateStructVector<binary::IncludeLocation>(builder, context.include_locations));
    });

    llvm::SmallVector<const Occurrence*> occurrence_keys;
    occurrence_keys.reserve(index->occurrences.size());
    auto occurrences = transform(index->occurrences, [&](auto&& value) {
        auto&& [occurrence, bitmap] = value;
        buffer.clear();
        buffer.resize_for_overwrite(bitmap.getSizeInBytes(false));
        bitmap.write(buffer.data(), false);
        occurrence_keys.emplace_back(&occurrence);
        return binary::CreateOccurrenceEntry(builder,
                                             safe_cast<binary::Occurrence>(&occurrence),
                                             CreateVector(builder, buffer));
    });
    ranges::sort(views::zip(occurrence_keys, occurrences), refl::less, [](auto e) -> auto& {
        return *std::get<0>(e);
    });

    llvm::SmallVector<std::uint64_t> relation_keys;
    relation_keys.reserve(index->relations.size());
    auto relations = transform(index->relations, [&](auto&& value) {
        auto&& [symbol_id, symbol_relations] = value;
        auto relations = transform(symbol_relations, [&](auto&& value) {
            auto&& [relation, bitmap] = value;
            buffer.clear();
            buffer.resize_for_overwrite(bitmap.getSizeInBytes(false));
            bitmap.write(buffer.data(), false);
            return binary::CreateRelationEntry(builder,
                                               safe_cast<binary::Relation>(&relation),
                                               CreateVector(builder, buffer));
        });
        relation_keys.emplace_back(symbol_id);
        return binary::CreateSymbolRelationsEntry(builder,
                                                  symbol_id,
                                                  CreateVector(builder, relations));
    });
    ranges::sort(views::zip(relation_keys, relations), refl::less, [](auto e) -> auto {
        return std::get<0>(e);
    });

    auto merged_index = binary::CreateMergedIndex(builder,
                                                  index->max_canonical_id,
                                                  CreateVector(builder, canonical_cache),
                                                  CreateVector(builder, header_contexts),
                                                  CreateVector(builder, compilation_contexts),
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
        auto index = fbs::GetRoot<binary::MergedIndex>(self.buffer->getBufferStart());
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
        auto it = self.impl->relations.find(symbol);
        if(it == self.impl->relations.end()) [[unlikely]] {
            return;
        }

        auto& relations = it->second;
        for(auto& [relation, _]: relations) {
            if(relation.kind & kind) {
                if(!callback(relation)) {
                    break;
                }
            }
        }
    } else if(self.buffer) {
        auto index = fbs::GetRoot<binary::MergedIndex>(self.buffer->getBufferStart());
        auto& entries = *index->relations();

        auto it = ranges::lower_bound(entries, symbol, {}, [](auto e) { return e->symbol(); });
        if(it == entries.end() || it->symbol() != symbol) [[unlikely]] {
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

bool MergedIndex::need_update(this const Self& self, llvm::ArrayRef<llvm::StringRef> path_mapping) {
    if(self.impl) {
        if(self.impl->compilation_contexts.empty()) {
            return true;
        }

        auto& context = self.impl->compilation_contexts.begin()->getSecond();

        llvm::DenseSet<std::uint32_t> deps;
        for(auto& location: context.include_locations) {
            auto [_, success] = deps.insert(location.path_id);
            if(success) {
                fs::file_status status;
                if(auto err = fs::status(path_mapping[location.path_id], status)) {
                    return true;
                }

                auto time = std::chrono::duration_cast<std::chrono::milliseconds>(
                    status.getLastModificationTime().time_since_epoch());
                if(time.count() > context.build_at) {
                    return true;
                }
            }
        }

        return false;
    } else if(self.buffer) {
        auto index = fbs::GetRoot<binary::MergedIndex>(self.buffer->getBufferStart());
        if(index->compilation_contexts()->empty()) {
            return true;
        }

        auto context = *index->compilation_contexts()->begin();

        llvm::DenseSet<std::uint32_t> deps;
        for(auto location: *context->include_locations()) {
            auto [_, success] = deps.insert(location->path_id());
            if(success) {
                fs::file_status status;
                if(auto err = fs::status(path_mapping[location->path_id()], status)) {
                    return true;
                }

                auto time = std::chrono::duration_cast<std::chrono::milliseconds>(
                    status.getLastModificationTime().time_since_epoch());
                if(time.count() > context->build_at()) {
                    return true;
                }
            }
        }

        return false;
    }

    return true;
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
                        std::chrono::milliseconds build_at,
                        std::vector<IncludeLocation> include_locations,
                        FileIndex& index) {
    self.load_in_memory();
    self.impl->merge(path_id, index, [&](Impl& self, std::uint32_t canonical_id) {
        auto& context = self.compilation_contexts[path_id];
        context.canonical_id = canonical_id;
        context.build_at = build_at.count();
        context.include_locations = std::move(include_locations);
    });
}

void MergedIndex::merge(this Self& self,
                        std::uint32_t path_id,
                        std::uint32_t include_id,
                        FileIndex& index) {
    self.load_in_memory();
    self.impl->merge(path_id, index, [&](Impl& self, std::uint32_t canonical_id) {
        auto& context = self.header_contexts[path_id];
        context.includes.emplace_back(include_id, canonical_id);
    });
}

bool operator==(MergedIndex& lhs, MergedIndex& rhs) {
    lhs.load_in_memory();
    rhs.load_in_memory();
    return *lhs.impl == *rhs.impl;
}

}  // namespace clice::index

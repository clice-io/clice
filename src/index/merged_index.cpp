#include "index/merged_index.h"

#include <cassert>
#include <cstdint>
#include <ranges>
#include <span>
#include <tuple>

#include "index/kotatsu_adapters.h"  // type_adapter specializations
#include "support/filesystem.h"

#include "kota/codec/flatbuffers/deserializer.h"
#include "kota/codec/flatbuffers/proxy.h"
#include "kota/codec/flatbuffers/serializer.h"
#include "kota/meta/annotation.h"
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

    std::uint64_t build_at = 0;

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

    /// Reference counts per canonical id — derivable from header/compilation
    /// contexts at load time, so it doesn't need to live on the wire.
    kota::meta::skip<std::vector<std::uint32_t>> canonical_ref_counts;

    /// The canonical id set of removed index.
    roaring::Roaring removed;

    /// All merged symbol occurrences.
    llvm::DenseMap<Occurrence, roaring::Roaring> occurrences;

    /// All merged symbol relations.
    llvm::DenseMap<SymbolHash, llvm::DenseMap<Relation, roaring::Roaring>> relations;

    /// Sorted occurrences cache for fast lookup — rebuilt on demand.
    kota::meta::skip<std::vector<Occurrence>> occurrences_cache;

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

namespace {

namespace kfb = kota::codec::flatbuffers;

std::span<const std::uint8_t> buffer_bytes(const llvm::MemoryBuffer& buffer) {
    return std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(buffer.getBufferStart()),
        buffer.getBufferSize());
}

}  // namespace

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

    auto bytes = buffer_bytes(*self.buffer);
    auto result = kfb::from_flatbuffer(bytes, *self.impl);
    if(!result) {
        self.buffer.reset();
        return;
    }

    // Rebuild the ref count table from the already-loaded contexts.
    auto& index = *self.impl;
    index.canonical_ref_counts.clear();
    index.canonical_ref_counts.resize(index.max_canonical_id, 0);
    for(auto& [_, ctx]: index.header_contexts) {
        for(auto& inc: ctx.includes) {
            index.canonical_ref_counts[inc.canonical_id] += 1;
        }
    }
    for(auto& [_, ctx]: index.compilation_contexts) {
        index.canonical_ref_counts[ctx.canonical_id] += 1;
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

    auto bytes = kfb::to_flatbuffer(*self.impl);
    assert(bytes && "MergedIndex flatbuffer serialization failed");
    out.write(reinterpret_cast<const char*>(bytes->data()), bytes->size());
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
            std::ranges::sort(occurrences, [](const Occurrence& lhs, const Occurrence& rhs) {
                return std::tuple(lhs.range.begin, lhs.range.end, lhs.target) <
                       std::tuple(rhs.range.begin, rhs.range.end, rhs.target);
            });
        }

        auto it = std::ranges::lower_bound(occurrences, offset, {}, [](index::Occurrence& o) {
            return o.range.end;
        });

        while(it != occurrences.end()) {
            if(it->range.contains(offset)) {
                // Skip occurrences whose canonical_ids are all removed.
                if(!index.removed.isEmpty()) {
                    auto bitmap_it = index.occurrences.find(*it);
                    if(bitmap_it != index.occurrences.end()) {
                        auto remaining = bitmap_it->second - index.removed;
                        if(remaining.isEmpty()) {
                            it++;
                            continue;
                        }
                    }
                }

                if(!callback(*it)) {
                    break;
                }

                it++;
                continue;
            }

            break;
        }
    } else if(self.buffer) {
        // Lazy path: binary-search the sorted occurrences array directly in
        // the flatbuffer without materializing the in-memory Impl.
        auto root = kfb::table_view<Impl>::from_bytes(buffer_bytes(*self.buffer));
        auto entries = root[&Impl::occurrences];

        auto read_occurrence = [](auto occ_view) -> Occurrence {
            auto range_view = occ_view[&Occurrence::range];
            return Occurrence{
                LocalSourceRange{range_view[&LocalSourceRange::begin],
                                 range_view[&LocalSourceRange::end]},
                occ_view[&Occurrence::target],
            };
        };

        const std::size_t count = entries.size();
        std::size_t lo = 0;
        std::size_t hi = count;
        while(lo < hi) {
            auto mid = lo + (hi - lo) / 2;
            auto entry = entries.at(mid);
            auto range_view = entry.template get<0>()[&Occurrence::range];
            if(range_view[&LocalSourceRange::end] < offset) {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }

        for(; lo < count; ++lo) {
            auto entry = entries.at(lo);
            auto occurrence = read_occurrence(entry.template get<0>());
            if(!occurrence.range.contains(offset)) {
                break;
            }
            if(!callback(occurrence)) {
                break;
            }
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
        for(auto& [relation, bitmap]: relations) {
            if(relation.kind & kind) {
                // Skip relations whose canonical_ids are all removed.
                if(!self.impl->removed.isEmpty()) {
                    auto remaining = bitmap - self.impl->removed;
                    if(remaining.isEmpty()) {
                        continue;
                    }
                }

                if(!callback(relation)) {
                    break;
                }
            }
        }
    } else if(self.buffer) {
        // Lazy path: binary-search the outer relations map and iterate the
        // inner map without materializing Impl.
        auto root = kfb::table_view<Impl>::from_bytes(buffer_bytes(*self.buffer));
        auto outer = root[&Impl::relations];
        auto entry = outer.find(symbol);
        if(!entry) {
            return;
        }
        auto inner = entry->template get<1>();
        const std::size_t count = inner.size();
        for(std::size_t i = 0; i < count; ++i) {
            auto rel_view = inner.at(i).template get<0>();
            // Kind comes back as the wire uint32 via the type_adapter; rewrap it.
            auto relation_kind =
                RelationKind(static_cast<RelationKind::Kind>(rel_view[&Relation::kind]));
            if(relation_kind & kind) {
                auto range_view = rel_view[&Relation::range];
                Relation relation{
                    .kind = relation_kind,
                    .padding = rel_view[&Relation::padding],
                    .range = LocalSourceRange{range_view[&LocalSourceRange::begin],
                                              range_view[&LocalSourceRange::end]},
                    .target_symbol = rel_view[&Relation::target_symbol],
                };
                if(!callback(relation)) {
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
        auto root = kfb::table_view<Impl>::from_bytes(buffer_bytes(*self.buffer));
        auto contexts = root[&Impl::compilation_contexts];
        if(contexts.empty()) {
            return true;
        }

        auto context = contexts.at(0).template get<1>();
        auto build_at = context[&CompilationContext::build_at];
        auto include_locations = context[&CompilationContext::include_locations];

        llvm::DenseSet<std::uint32_t> deps;
        const std::size_t count = include_locations.size();
        for(std::size_t i = 0; i < count; ++i) {
            auto location = include_locations.at(i);
            auto path_id = location[&IncludeLocation::path_id];
            auto [_, success] = deps.insert(path_id);
            if(success) {
                fs::file_status status;
                if(auto err = fs::status(path_mapping[path_id], status)) {
                    return true;
                }

                auto time = std::chrono::duration_cast<std::chrono::milliseconds>(
                    status.getLastModificationTime().time_since_epoch());
                if(time.count() > build_at) {
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

    // Handle header context removal.
    auto hc_it = index.header_contexts.find(path_id);
    if(hc_it != index.header_contexts.end()) {
        for(auto& [_, canonical_id]: hc_it->second.includes) {
            auto& ref_counts = index.canonical_ref_counts[canonical_id];
            ref_counts -= 1;
            if(ref_counts == 0) {
                index.removed.add(canonical_id);
            }
        }
        index.header_contexts.erase(hc_it);
    }

    // Handle compilation context removal.
    auto cc_it = index.compilation_contexts.find(path_id);
    if(cc_it != index.compilation_contexts.end()) {
        auto canonical_id = cc_it->second.canonical_id;
        auto& ref_counts = index.canonical_ref_counts[canonical_id];
        ref_counts -= 1;
        if(ref_counts == 0) {
            index.removed.add(canonical_id);
        }
        index.compilation_contexts.erase(cc_it);
    }

    // Invalidate cached occurrences.
    index.occurrences_cache.clear();
}

void MergedIndex::merge(this Self& self,
                        std::uint32_t path_id,
                        std::chrono::milliseconds build_at,
                        std::vector<IncludeLocation> include_locations,
                        FileIndex& index,
                        llvm::StringRef content) {
    self.load_in_memory();
    self.impl->content = content.str();
    self.impl->merge(path_id, index, [&](Impl& self, std::uint32_t canonical_id) {
        auto& context = self.compilation_contexts[path_id];
        context.canonical_id = canonical_id;
        context.build_at = build_at.count();
        context.include_locations = std::move(include_locations);
    });
    self.impl->occurrences_cache.clear();
}

void MergedIndex::merge(this Self& self,
                        std::uint32_t path_id,
                        std::uint32_t include_id,
                        FileIndex& index,
                        llvm::StringRef content) {
    self.load_in_memory();
    if(self.impl->content.empty() && !content.empty()) {
        self.impl->content = content.str();
    }
    self.impl->merge(path_id, index, [&](Impl& self, std::uint32_t canonical_id) {
        auto& context = self.header_contexts[path_id];
        context.includes.emplace_back(include_id, canonical_id);
    });
    self.impl->occurrences_cache.clear();
}

llvm::StringRef MergedIndex::content(this const Self& self) {
    if(self.impl) {
        return self.impl->content;
    } else if(self.buffer) {
        auto root = kfb::table_view<Impl>::from_bytes(buffer_bytes(*self.buffer));
        auto view = root[&Impl::content];
        return llvm::StringRef(view.data(), view.size());
    }
    return {};
}

bool operator==(MergedIndex& lhs, MergedIndex& rhs) {
    lhs.load_in_memory();
    rhs.load_in_memory();
    return *lhs.impl == *rhs.impl;
}

}  // namespace clice::index

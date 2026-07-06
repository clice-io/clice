#include "index/merged_index.h"

#include <ranges>
#include <tuple>

#include "index/path_pool.h"
#include "index/serialization.h"
#include "support/filesystem.h"

#include "kota/ipc/lsp/position.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/Support/raw_os_ostream.h"
#include "llvm/Support/xxhash.h"

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

/// (path_id, content_hash) captured for one dependency at index-build time.
struct DepHash {
    std::uint32_t path_id;
    std::uint64_t content_hash;

    friend bool operator==(const DepHash&, const DepHash&) = default;
};

namespace {

/// Hash a file's content with the same scheme the server layer uses for its
/// dependency snapshots (`workspace::hash_file`). Returns 0 on read failure.
std::uint64_t hash_file(llvm::StringRef path) {
    auto buffer = llvm::MemoryBuffer::getFile(path);
    if(!buffer) {
        return 0;
    }
    return llvm::xxh3_64bits((*buffer)->getBuffer());
}

/// Two-layer staleness test for a single dependency, mirroring the server's
/// `deps_changed`: Layer 1 trusts an unchanged mtime (no file read); Layer 2
/// re-hashes a file whose mtime moved and treats a matching hash as a mere
/// touch, not a real edit.
bool dep_stale(llvm::StringRef path,
               std::uint64_t build_at,
               std::optional<std::uint64_t> stored_hash) {
    fs::file_status status;
    if(auto err = fs::status(path, status)) {
        return true;
    }

    auto mtime = std::chrono::duration_cast<std::chrono::milliseconds>(
        status.getLastModificationTime().time_since_epoch());
    if(mtime.count() <= static_cast<std::int64_t>(build_at)) {
        return false;
    }

    // mtime moved: without a baseline hash we cannot prove the content is
    // unchanged, so fall back to the conservative rebuild.
    if(!stored_hash) {
        return true;
    }
    // A matching hash means the file was only touched, not edited. We do NOT
    // refresh the stored mtime baseline on a match: the baseline lives inside
    // the immutable serialized shard, and updating it would mean re-serializing
    // the whole shard just to skip a rebuild. So a touched-but-unchanged file is
    // re-hashed on every check until a real edit forces a genuine reindex — a
    // cheap single read, far cheaper than a needless full reindex.
    return hash_file(path) != *stored_hash;
}

}  // namespace

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

    /// Content hash of each distinct dependency (first-seen order), used by
    /// the Layer 2 staleness check to distinguish a real edit from a touch.
    llvm::SmallVector<DepHash> dep_hashes;

    friend bool operator==(const CompilationContext&, const CompilationContext&) = default;
};

struct MergedIndex::Impl {
    /// On-disk schema version, serialized first so loaders can reject
    /// incompatible shards before touching anything else.
    std::uint32_t format_version = index_format_version;

    /// Shard-local path table: every path id stored in this shard indexes
    /// into it, so shards are self-contained across sessions (runtime pool
    /// ids never persist).
    PathPool paths;

    /// The content of corresponding source file.
    std::string content;

    /// Line start offsets for position mapping.
    std::vector<std::uint32_t> line_starts;

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

    /// The reference count of each canonical id. Derived state: rebuilt from
    /// the contexts on load, never serialized.
    kota::meta::skip<std::vector<std::uint32_t>> canonical_ref_counts;

    /// The canonical id set of removed index.
    roaring::Roaring removed;

    /// All merged symbol occurrences.
    llvm::DenseMap<Occurrence, roaring::Roaring> occurrences;

    /// All merged symbol relations.
    llvm::DenseMap<SymbolHash, llvm::DenseMap<Relation, roaring::Roaring>> relations;

    /// Symbols local to this file (FileLocal) or TU (TULocal).
    SymbolTable symbols;

    /// Sorted occurrences cache for fast lookup. Derived state, never
    /// serialized.
    kota::meta::skip<std::vector<Occurrence>> occurrences_cache;

    /// Drop one reference to a canonical index; the last reference masks its
    /// occurrences and relations via the removed bitmap. A later re-merge of
    /// identical content resurrects the id instead of re-adding rows.
    void release_canonical(this Impl& self, std::uint32_t canonical_id) {
        auto& ref_count = self.canonical_ref_counts[canonical_id];
        ref_count -= 1;
        if(ref_count == 0) {
            self.removed.add(canonical_id);
        }
    }

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

    /// Vacuum state masked by the removed bitmap so the reflected object can
    /// be serialized as-is: rows whose every canonical was released are
    /// dropped, live bitmaps are pre-subtracted, dead cache entries go with
    /// them (a later re-merge of identical content mints a fresh canonical),
    /// and the removed bitmap ends up empty. This matches exactly what a
    /// load of the compacted bytes would reconstruct.
    void compact(this Impl& self) {
        self.occurrences_cache.clear();

        auto& removed = self.removed;
        if(removed.isEmpty()) {
            return;
        }

        llvm::SmallVector<llvm::SmallString<64>> dead_hashes;
        for(auto& entry: self.canonical_cache) {
            if(removed.contains(entry.second)) {
                dead_hashes.emplace_back(entry.first());
            }
        }
        for(auto& hash: dead_hashes) {
            self.canonical_cache.erase(hash);
        }

        llvm::DenseMap<Occurrence, roaring::Roaring> live_occurrences;
        for(auto& [occurrence, bitmap]: self.occurrences) {
            auto masked = bitmap - removed;
            if(!masked.isEmpty()) {
                live_occurrences.try_emplace(occurrence, std::move(masked));
            }
        }
        self.occurrences = std::move(live_occurrences);

        llvm::DenseMap<SymbolHash, llvm::DenseMap<Relation, roaring::Roaring>> live_relations;
        for(auto& [symbol_id, symbol_relations]: self.relations) {
            llvm::DenseMap<Relation, roaring::Roaring> live;
            for(auto& [relation, bitmap]: symbol_relations) {
                auto masked = bitmap - removed;
                if(!masked.isEmpty()) {
                    live.try_emplace(relation, std::move(masked));
                }
            }
            if(!live.empty()) {
                live_relations.try_emplace(symbol_id, std::move(live));
            }
        }
        self.relations = std::move(live_relations);

        removed = roaring::Roaring();
    }

    friend bool operator==(const Impl&, const Impl&) = default;
};

namespace {

std::span<const std::byte> buffer_bytes(const llvm::MemoryBuffer& buffer) {
    return {reinterpret_cast<const std::byte*>(buffer.getBufferStart()), buffer.getBufferSize()};
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

    auto& index = *self.impl;

    if(!read_flatbuffer(self.buffer->getBufferStart(), self.buffer->getBufferSize(), index)) {
        index = Impl();
        self.buffer.reset();
        return;
    }

    // Rebuild the derived reference counts from the decoded contexts. Ids are
    // bounds-checked: the verifier guarantees structure, not semantics, so a
    // corrupted-but-well-formed shard must not index out of range.
    index.canonical_ref_counts.resize(index.max_canonical_id, 0);
    auto count_ref = [&](std::uint32_t canonical_id) {
        if(canonical_id < index.canonical_ref_counts.size()) {
            index.canonical_ref_counts[canonical_id] += 1;
        }
    };
    for(auto& [_, context]: index.header_contexts) {
        for(auto& include: context.includes) {
            count_ref(include.canonical_id);
        }
    }
    for(auto& [_, context]: index.compilation_contexts) {
        count_ref(context.canonical_id);
    }

    if(index.line_starts.empty() && !index.content.empty()) {
        index.line_starts = kota::ipc::lsp::build_line_starts(index.content);
    }

    self.buffer.reset();
}

MergedIndex MergedIndex::load(llvm::StringRef path) {
    auto buffer = llvm::MemoryBuffer::getFile(path);
    if(!buffer) {
        return MergedIndex();
    }

    // A stale cache directory from an older build must never crash the server
    // or be misread. Deep-verify the blob against the reflected shard schema
    // (flatc-era blobs already fail the kotatsu buffer identifier check),
    // then discard any shard whose format version differs. A discarded shard
    // is treated as "not on disk" and the background indexer rebuilds it.
    auto bytes = buffer_bytes(**buffer);
    if(!kota::codec::fbs::verify_flatbuffer<Impl>(bytes)) {
        return MergedIndex();
    }

    auto root = kota::codec::fbs::table_view<Impl>::from_bytes(bytes);
    if(root[&Impl::format_version] != index_format_version) {
        return MergedIndex();
    }

    return MergedIndex(std::move(*buffer), nullptr);
}

void MergedIndex::serialize(this Self& self, llvm::raw_ostream& out) {
    if(self.buffer) {
        out.write(self.buffer->getBufferStart(), self.buffer->getBufferSize());
        return;
    }

    if(!self.impl) {
        return;
    }

    // The serialized shard is served through buffer-only lookups that never
    // consult the removed bitmap, so masked state must not reach disk.
    // Compacting in place produces exactly the state a reload of the written
    // bytes would reconstruct, after which the reflected object encodes
    // as-is. Map entries are written sorted by their keys' ordering, which
    // the lazy lookups binary-search.
    self.impl->compact();
    write_flatbuffer(out, *self.impl);
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
            std::ranges::sort(occurrences);
        }

        lookup_occurrences(occurrences, offset, [&](const Occurrence& occurrence) {
            // Skip occurrences whose canonical_ids are all removed.
            if(!index.removed.isEmpty()) {
                auto bitmap_it = index.occurrences.find(occurrence);
                if(bitmap_it != index.occurrences.end() &&
                   (bitmap_it->second - index.removed).isEmpty()) {
                    return true;
                }
            }
            return callback(occurrence);
        });
    } else if(self.buffer) {
        auto root = kota::codec::fbs::table_view<Impl>::from_bytes(buffer_bytes(*self.buffer));
        auto occurrences = root[&Impl::occurrences];

        // Entries are sorted by Occurrence's (begin, end, target) ordering;
        // mirror the in-memory lower_bound on range.end.
        std::size_t lo = 0;
        std::size_t hi = occurrences.size();
        while(lo < hi) {
            auto mid = lo + (hi - lo) / 2;
            if(occurrences.at(mid).get<0>().range.end < offset) {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }

        for(std::size_t i = lo; i < occurrences.size(); ++i) {
            auto occurrence = occurrences.at(i).get<0>();
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
        auto root = kota::codec::fbs::table_view<Impl>::from_bytes(buffer_bytes(*self.buffer));
        auto entry = root[&Impl::relations].find(symbol);
        if(!entry) [[unlikely]] {
            return;
        }

        auto relations = entry->get<1>();
        for(std::size_t i = 0; i < relations.size(); ++i) {
            auto view = relations.at(i).get<0>();
            Relation relation{
                .kind = RelationKind(static_cast<RelationKind::Kind>(view[&Relation::kind])),
                .padding = view[&Relation::padding],
                .range = view[&Relation::range],
                .target_symbol = view[&Relation::target_symbol],
            };
            if(relation.kind & kind) {
                if(!callback(relation)) {
                    break;
                }
            }
        }
    }
}

bool MergedIndex::need_update(this const Self& self) {
    if(self.impl) {
        if(self.impl->compilation_contexts.empty()) {
            return true;
        }

        auto& context = self.impl->compilation_contexts.begin()->getSecond();
        auto& paths = self.impl->paths.paths;

        llvm::DenseMap<std::uint32_t, std::uint64_t> hashes;
        for(auto& dep: context.dep_hashes) {
            hashes.try_emplace(dep.path_id, dep.content_hash);
        }

        llvm::DenseSet<std::uint32_t> deps;
        for(auto& location: context.include_locations) {
            if(!deps.insert(location.path_id).second) {
                continue;
            }
            // A dep the table does not cover cannot be validated: rebuild.
            if(location.path_id >= paths.size()) {
                return true;
            }
            auto it = hashes.find(location.path_id);
            if(dep_stale(paths[location.path_id],
                         context.build_at,
                         it != hashes.end() ? std::optional(it->second) : std::nullopt)) {
                return true;
            }
        }

        return false;
    } else if(self.buffer) {
        auto root = kota::codec::fbs::table_view<Impl>::from_bytes(buffer_bytes(*self.buffer));
        auto contexts = root[&Impl::compilation_contexts];
        if(contexts.empty()) {
            return true;
        }

        auto context = contexts.at(0).get<1>();
        auto paths = root[&Impl::paths];

        llvm::DenseMap<std::uint32_t, std::uint64_t> hashes;
        auto dep_hashes = context[&CompilationContext::dep_hashes];
        for(std::size_t i = 0; i < dep_hashes.size(); ++i) {
            auto dep = dep_hashes.at(i);
            hashes.try_emplace(dep.path_id, dep.content_hash);
        }

        auto build_at = context[&CompilationContext::build_at];
        auto locations = context[&CompilationContext::include_locations];
        llvm::DenseSet<std::uint32_t> deps;
        for(std::size_t i = 0; i < locations.size(); ++i) {
            auto location = locations.at(i);
            if(!deps.insert(location.path_id).second) {
                continue;
            }
            // A dep the table does not cover cannot be validated: rebuild.
            if(location.path_id >= paths.size()) {
                return true;
            }
            auto it = hashes.find(location.path_id);
            if(dep_stale(llvm::StringRef(paths.at(location.path_id)),
                         build_at,
                         it != hashes.end() ? std::optional(it->second) : std::nullopt)) {
                return true;
            }
        }

        return false;
    }

    return true;
}

bool MergedIndex::has_contribution(this const Self& self, llvm::StringRef context_path) {
    // Match the path table's normalization so Windows separators compare.
    llvm::SmallString<256> normalized;
    if(context_path.contains('\\')) {
        normalized = context_path;
        std::replace(normalized.begin(), normalized.end(), '\\', '/');
        context_path = normalized;
    }

    if(self.impl) {
        auto it = self.impl->paths.find(context_path);
        if(it == self.impl->paths.cache.end()) {
            return false;
        }
        return self.impl->header_contexts.contains(it->second) ||
               self.impl->compilation_contexts.contains(it->second);
    }

    if(self.buffer) {
        auto root = kota::codec::fbs::table_view<Impl>::from_bytes(buffer_bytes(*self.buffer));
        auto paths = root[&Impl::paths];
        std::optional<std::uint32_t> local;
        for(std::uint32_t i = 0; i < paths.size(); ++i) {
            if(llvm::StringRef(paths.at(i)) == context_path) {
                local = i;
                break;
            }
        }
        if(!local) {
            return false;
        }
        return root[&Impl::header_contexts].contains(*local) ||
               root[&Impl::compilation_contexts].contains(*local);
    }

    return false;
}

void MergedIndex::remove(this Self& self, llvm::StringRef context_path) {
    self.load_in_memory();
    auto& index = *self.impl;

    auto path_it = index.paths.find(context_path);
    if(path_it == index.paths.cache.end()) {
        return;
    }
    auto path_id = path_it->second;

    // Handle header context removal.
    auto hc_it = index.header_contexts.find(path_id);
    if(hc_it != index.header_contexts.end()) {
        for(auto& [_, canonical_id]: hc_it->second.includes) {
            index.release_canonical(canonical_id);
        }
        index.header_contexts.erase(hc_it);
    }

    // Handle compilation context removal.
    auto cc_it = index.compilation_contexts.find(path_id);
    if(cc_it != index.compilation_contexts.end()) {
        index.release_canonical(cc_it->second.canonical_id);
        index.compilation_contexts.erase(cc_it);
    }

    // Invalidate cached occurrences.
    index.occurrences_cache.clear();
}

bool MergedIndex::find_symbol(this const Self& self,
                              SymbolHash hash,
                              std::string& name,
                              SymbolKind& kind) {
    if(self.impl) {
        auto it = self.impl->symbols.find(hash);
        if(it != self.impl->symbols.end()) {
            name = it->second.name;
            kind = it->second.kind;
            return true;
        }
    } else if(self.buffer) {
        auto root = kota::codec::fbs::table_view<Impl>::from_bytes(buffer_bytes(*self.buffer));
        if(auto entry = root[&Impl::symbols].find(hash)) {
            auto symbol = entry->get<1>();
            name = std::string(symbol[&Symbol::name]);
            kind = SymbolKind(static_cast<SymbolKind::Kind>(symbol[&Symbol::kind]));
            return true;
        }
    }
    return false;
}

void MergedIndex::merge_symbols(this Self& self, const SymbolTable& symbols) {
    self.load_in_memory();
    for(auto& [hash, symbol]: symbols) {
        auto [it, inserted] = self.impl->symbols.try_emplace(hash);
        if(inserted) {
            it->second.name = symbol.name;
            it->second.kind = symbol.kind;
            it->second.scope = symbol.scope;
        }
    }
}

void MergedIndex::merge(this Self& self,
                        llvm::StringRef tu_path,
                        std::chrono::milliseconds build_at,
                        llvm::ArrayRef<DepLocation> deps,
                        FileIndex& index,
                        llvm::StringRef content) {
    self.load_in_memory();
    self.impl->content = content.str();
    self.impl->line_starts = kota::ipc::lsp::build_line_starts(self.impl->content);

    // Intern the dependencies into the shard's own path table, and capture a
    // content hash for each distinct one so a later staleness check can tell
    // a real edit apart from a mere touch (mtime bumped, bytes unchanged).
    // Only re-hashing at check time can prove that, so the baseline is
    // recorded here.
    // TODO: this re-reads every dependency on the event-loop thread even
    // though the indexer worker already read them; if it shows up on large
    // cold-start profiles, have the worker ship the hashes in the TUIndex.
    std::vector<IncludeLocation> include_locations;
    llvm::SmallVector<DepHash> dep_hashes;
    llvm::DenseSet<std::uint32_t> seen;
    include_locations.reserve(deps.size());
    for(auto& dep: deps) {
        auto local_id = self.impl->paths.path_id(dep.path);
        include_locations.push_back({local_id, dep.line, dep.include_id});
        if(!seen.insert(local_id).second) {
            continue;
        }
        // A dep modified after the indexed snapshot was built must not
        // contribute a baseline: hashing it now would bless content the
        // rows were never built from, hiding the edit forever. With no
        // stored hash the staleness check stays conservative and the TU
        // simply reindexes once more.
        fs::file_status status;
        if(auto err = fs::status(dep.path, status)) {
            continue;
        }
        auto mtime = std::chrono::duration_cast<std::chrono::milliseconds>(
            status.getLastModificationTime().time_since_epoch());
        if(mtime <= build_at) {
            dep_hashes.emplace_back(local_id, hash_file(dep.path));
        }
    }

    auto path_id = self.impl->paths.path_id(tu_path);
    self.impl->merge(path_id, index, [&](Impl& self, std::uint32_t canonical_id) {
        // A reindex of the same TU replaces its previous contribution:
        // without the release, the old canonical's occurrences and relations
        // stay live and queries serve pre-edit state alongside the new one.
        auto [it, inserted] = self.compilation_contexts.try_emplace(path_id);
        if(!inserted) {
            self.release_canonical(it->second.canonical_id);
        }
        auto& context = it->second;
        context.canonical_id = canonical_id;
        context.build_at = build_at.count();
        context.include_locations = std::move(include_locations);
        context.dep_hashes = std::move(dep_hashes);
    });
    self.impl->occurrences_cache.clear();
}

void MergedIndex::merge(this Self& self,
                        llvm::StringRef tu_path,
                        std::uint32_t include_id,
                        FileIndex& index,
                        llvm::StringRef content) {
    self.load_in_memory();
    auto path_id = self.impl->paths.path_id(tu_path);
    // The stored content is the position-mapping truth for this file; a
    // reindex after an edit must refresh it, not just fill it once.
    if(!content.empty() && self.impl->content != content) {
        self.impl->content = content.str();
        self.impl->line_starts = kota::ipc::lsp::build_line_starts(self.impl->content);
    }
    self.impl->merge(path_id, index, [&](Impl& self, std::uint32_t canonical_id) {
        // Keyed by the including TU: a reindex of that TU replaces its
        // previous contribution to this file wholesale, while contributions
        // from other TUs stay untouched.
        auto [it, inserted] = self.header_contexts.try_emplace(path_id);
        if(!inserted) {
            for(auto& [_, old_canonical]: it->second.includes) {
                self.release_canonical(old_canonical);
            }
            it->second.includes.clear();
        }
        it->second.includes.emplace_back(include_id, canonical_id);
    });
    self.impl->occurrences_cache.clear();
}

llvm::StringRef MergedIndex::content(this const Self& self) {
    if(self.impl) {
        return self.impl->content;
    } else if(self.buffer) {
        auto root = kota::codec::fbs::table_view<Impl>::from_bytes(buffer_bytes(*self.buffer));
        auto content = root[&Impl::content];
        return llvm::StringRef(content.data(), content.size());
    }
    return {};
}

std::span<const std::uint32_t> MergedIndex::line_starts(this const Self& self) {
    if(self.impl) {
        return self.impl->line_starts;
    } else if(self.buffer) {
        auto root = kota::codec::fbs::table_view<Impl>::from_bytes(buffer_bytes(*self.buffer));
        auto line_starts = root[&Impl::line_starts];
        if(auto* vector = line_starts.raw(); vector != nullptr && vector->size() > 0) {
            return {vector->data(), vector->size()};
        }
    }
    return {};
}

bool operator==(MergedIndex& lhs, MergedIndex& rhs) {
    lhs.load_in_memory();
    rhs.load_in_memory();
    return *lhs.impl == *rhs.impl;
}

}  // namespace clice::index

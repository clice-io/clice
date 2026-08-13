#include "index/merged_index.h"

#include <atomic>
#include <ranges>
#include <string>
#include <tuple>

#include "compile/dep_file.h"
#include "index/path_pool.h"
#include "index/serialization.h"
#include "support/filesystem.h"

#include "kota/ipc/lsp/position.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
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
            .kind = clice::RelationKind::Invalid,
            .range = clice::LocalSourceRange(-1, 0),
            .target_symbol = 0,
        };
    }

    inline static R getTombstoneKey() {
        return R{
            .kind = clice::RelationKind::Invalid,
            .range = clice::LocalSourceRange(-2, 0),
            .target_symbol = 0,
        };
    }

    /// Contextual doesn’t take part in hashing and equality.
    static auto getHashValue(const R& relation) {
        return dense_hash(static_cast<std::uint32_t>(relation.kind),
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

/// Stat fast path for one dependency, recorded at merge time only when the
/// file provably did not change since before the indexed build started.
/// The server's mutable, in-place-repairing form of the same fast path is
/// `DepState` (server/state/workspace.h).
struct DepStamp {
    std::uint32_t path_id;
    std::uint64_t size;
    std::int64_t mtime_ns;

    friend bool operator==(const DepStamp&, const DepStamp&) = default;
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
/// `deps_changed`: Layer 1 trusts a stat EQUAL to the recorded stamp (no
/// file read) — equality, not a watermark, so backdated or preserved mtimes
/// cannot masquerade as fresh; Layer 2 re-hashes the disk against the
/// consumed-content hash and treats a match as a mere touch, not an edit.
bool dep_stale(llvm::StringRef path,
               const std::optional<DepStamp>& stamp,
               std::optional<std::uint64_t> stored_hash) {
    fs::file_status status;
    if(auto err = fs::status(path, status)) {
        return true;
    }

    if(stamp && stamp->size == status.getSize() && stamp->mtime_ns == fs::mtime_ns(status)) {
        return false;
    }

    // The stat moved (or no stamp was recorded): without a baseline hash we
    // cannot prove the content unchanged, so fall back to the conservative
    // rebuild.
    if(!stored_hash) {
        return true;
    }
    // A matching hash means the file was only touched, not edited. We do NOT
    // refresh the stored stamp on a match: the baseline lives inside the
    // immutable serialized shard, and updating it would mean re-serializing
    // the whole shard just to skip a rebuild. So a touched-but-unchanged file
    // is re-hashed on every check until a real edit forces a genuine reindex
    // — a cheap single read, far cheaper than a needless full reindex.
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

    /// Consumed-content hash of each distinct dependency (first-seen order),
    /// used by the Layer 2 staleness check to distinguish an edit from a
    /// touch. Reported by the indexing worker from the compiler's buffers.
    llvm::SmallVector<DepHash> dep_hashes;

    /// Stat fast paths for the dependencies that provably did not change
    /// since before the indexed build started (sparse, like dep_hashes).
    llvm::SmallVector<DepStamp> dep_stamps;

    friend bool operator==(const CompilationContext&, const CompilationContext&) = default;
};

struct MergedIndex::Impl {
    /// On-disk shard schema version (index_format_version), stamped by
    /// serialize() and gated by load(); version-less blobs from older builds
    /// read back as 0.
    std::uint32_t format_version = 0;

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
    /// the context tables when a blob loads in memory.
    KOTATSU_ANNOTATE(skip = true)
    <std::vector<std::uint32_t>> canonical_ref_counts;

    /// The canonical id set of removed index. Never persisted: compact()
    /// erases the masked rows for real before a shard reaches disk.
    KOTATSU_ANNOTATE(skip = true)
    <roaring::Roaring> removed;

    /// All merged symbol occurrences.
    llvm::DenseMap<Occurrence, roaring::Roaring> occurrences;

    /// All merged symbol relations.
    llvm::DenseMap<SymbolHash, llvm::DenseMap<Relation, roaring::Roaring>> relations;

    /// Symbols local to this file (FileLocal) or TU (TULocal).
    SymbolTable symbols;

    /// Sorted occurrences cache for fast lookup.
    KOTATSU_ANNOTATE(skip = true)
    <std::vector<Occurrence>> occurrences_cache;

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

    /// Erase the rows masked by the removed bitmap for real. Queries are
    /// unaffected (masked rows were already invisible), but a later re-merge
    /// of identical content mints a fresh canonical instead of resurrecting
    /// the id — the same behavior a save/load cycle produces.
    void compact(this Impl& self) {
        if(self.removed.isEmpty()) {
            return;
        }

        llvm::SmallVector<llvm::StringRef> dead_hashes;
        for(const auto& entry: self.canonical_cache) {
            if(self.removed.contains(entry.getValue())) {
                dead_hashes.push_back(entry.getKey());
            }
        }
        for(auto hash: dead_hashes) {
            self.canonical_cache.erase(hash);
        }

        llvm::SmallVector<Occurrence> dead_occurrences;
        for(auto& [occurrence, bitmap]: self.occurrences) {
            bitmap -= self.removed;
            if(bitmap.isEmpty()) {
                dead_occurrences.push_back(occurrence);
            }
        }
        for(auto& occurrence: dead_occurrences) {
            self.occurrences.erase(occurrence);
        }

        llvm::SmallVector<SymbolHash> dead_symbols;
        for(auto& [symbol, entries]: self.relations) {
            llvm::SmallVector<Relation> dead_relations;
            for(auto& [relation, bitmap]: entries) {
                bitmap -= self.removed;
                if(bitmap.isEmpty()) {
                    dead_relations.push_back(relation);
                }
            }
            for(auto& relation: dead_relations) {
                entries.erase(relation);
            }
            if(entries.empty()) {
                dead_symbols.push_back(symbol);
            }
        }
        for(auto symbol: dead_symbols) {
            self.relations.erase(symbol);
        }

        self.removed = roaring::Roaring();
        self.occurrences_cache.clear();
    }

    friend bool operator==(const Impl&, const Impl&) = default;
};

namespace {

using ShardView = kota::codec::fbs::table_view<MergedIndex::Impl>;

/// The blob was fully verified at load(); per-query views skip that cost.
ShardView root_of(const llvm::MemoryBuffer& buffer) {
    return ShardView::from_verified_bytes(blob_bytes(buffer.getBuffer()));
}

}  // namespace

MergedIndex::MergedIndex(std::unique_ptr<llvm::MemoryBuffer> buffer, std::unique_ptr<Impl> impl) :
    buffer(std::move(buffer)), impl(std::move(impl)) {}

/// Revision values come from one process-wide monotonic source, so a value
/// never repeats across shard objects: an entry erased and re-created at
/// the same key while a save's commit is in flight cannot alias the
/// revision that save snapshotted (a per-object counter restarting at the
/// same small numbers could).
static std::uint64_t next_revision() {
    static std::atomic<std::uint64_t> counter{0};
    return counter.fetch_add(1, std::memory_order_relaxed) + 1;
}

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
    // The buffer's structure was verified at load(), but structural
    // verification does not constrain field values: a canonical id at or
    // past max_canonical_id would index canonical_ref_counts out of bounds
    // below (and in every later release_canonical). A blob carrying one —
    // like a blob that fails to decode outright — is dropped, so the shard
    // reads as empty and the background indexer rebuilds it.
    auto usable = [&] {
        if(!deserialize_blob(self.buffer->getBuffer(), index)) {
            return false;
        }
        auto in_range = [&](std::uint32_t canonical_id) {
            return canonical_id < index.max_canonical_id;
        };
        for(const auto& entry: index.canonical_cache) {
            if(!in_range(entry.getValue())) {
                return false;
            }
        }
        for(auto& context: llvm::make_second_range(index.header_contexts)) {
            for(auto& include: context.includes) {
                if(!in_range(include.canonical_id)) {
                    return false;
                }
            }
        }
        for(auto& context: llvm::make_second_range(index.compilation_contexts)) {
            if(!in_range(context.canonical_id)) {
                return false;
            }
        }
        return true;
    };
    if(!usable()) {
        self.impl = std::make_unique<MergedIndex::Impl>();
        self.buffer.reset();
        return;
    }

    index.canonical_ref_counts.resize(index.max_canonical_id, 0);

    for(auto& context: llvm::make_second_range(index.header_contexts)) {
        for(auto& include: context.includes) {
            index.canonical_ref_counts[include.canonical_id] += 1;
        }
    }
    for(auto& context: llvm::make_second_range(index.compilation_contexts)) {
        index.canonical_ref_counts[context.canonical_id] += 1;
    }

    self.buffer.reset();
}

MergedIndex MergedIndex::load(llvm::StringRef path) {
    auto buffer = llvm::MemoryBuffer::getFile(path);
    if(!buffer) {
        return MergedIndex();
    }

    // A stale cache directory from an older build must never crash the
    // server or be misread. from_bytes deep-verifies every offset, string,
    // vector and table the views can reach; shards whose format version
    // differs are discarded (version-less shards read back 0). A discarded
    // shard is treated as "not on disk" and the background indexer rebuilds
    // it.
    auto root = ShardView::from_bytes(blob_bytes((*buffer)->getBuffer()));
    if(!root.valid() || root[&Impl::format_version] != index_format_version) {
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
    // consult the removed bitmap, so masked state must not reach disk at
    // all: compact first, then reflect the impl directly onto the wire.
    self.impl->compact();
    self.impl->format_version = index_format_version;
    serialize_blob(*self.impl, out);
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
        auto occurrences = root_of(*self.buffer)[&Impl::occurrences];
        scan_occurrences_at(
            occurrences.size(),
            offset,
            [&](std::size_t i) { return occurrences.at(i).get<0>(); },
            callback);
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
            if(RelationKind(relation.kind) & kind) {
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
        auto found = root_of(*self.buffer)[&Impl::relations].find(symbol);
        if(!found) [[unlikely]] {
            return;
        }

        auto entries = found->get<1>();
        for(std::size_t i = 0; i < entries.size(); ++i) {
            Relation relation = entries.at(i).get<0>();
            if(RelationKind(relation.kind) & kind) {
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

        auto& paths = self.impl->paths.paths;

        // Every context must be validated: shards normally hold one, but
        // whichever a partial iteration skipped would keep serving stale
        // rows behind a fresh verdict.
        for(auto& entry: self.impl->compilation_contexts) {
            auto& context = entry.getSecond();

            llvm::DenseMap<std::uint32_t, std::uint64_t> hashes;
            for(auto& dep: context.dep_hashes) {
                hashes.try_emplace(dep.path_id, dep.content_hash);
            }
            llvm::DenseMap<std::uint32_t, DepStamp> stamps;
            for(auto& stamp: context.dep_stamps) {
                stamps.try_emplace(stamp.path_id, stamp);
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
                auto stamp_it = stamps.find(location.path_id);
                auto it = hashes.find(location.path_id);
                if(dep_stale(paths[location.path_id],
                             stamp_it != stamps.end() ? std::optional(stamp_it->second)
                                                      : std::nullopt,
                             it != hashes.end() ? std::optional(it->second) : std::nullopt)) {
                    return true;
                }
            }
        }

        return false;
    } else if(self.buffer) {
        auto root = root_of(*self.buffer);
        auto contexts = root[&Impl::compilation_contexts];
        if(contexts.empty()) {
            return true;
        }

        auto paths = root[&Impl::paths];

        for(std::size_t c = 0; c < contexts.size(); ++c) {
            auto context = contexts.at(c).get<1>();

            llvm::DenseMap<std::uint32_t, std::uint64_t> hashes;
            auto dep_hashes = context[&CompilationContext::dep_hashes];
            for(std::size_t i = 0; i < dep_hashes.size(); ++i) {
                DepHash dep = dep_hashes[i];
                hashes.try_emplace(dep.path_id, dep.content_hash);
            }
            llvm::DenseMap<std::uint32_t, DepStamp> stamps;
            auto dep_stamps = context[&CompilationContext::dep_stamps];
            for(std::size_t i = 0; i < dep_stamps.size(); ++i) {
                DepStamp stamp = dep_stamps[i];
                stamps.try_emplace(stamp.path_id, stamp);
            }

            llvm::DenseSet<std::uint32_t> deps;
            auto locations = context[&CompilationContext::include_locations];
            for(std::size_t i = 0; i < locations.size(); ++i) {
                IncludeLocation location = locations[i];
                if(!deps.insert(location.path_id).second) {
                    continue;
                }
                // A dep the table does not cover cannot be validated: rebuild.
                if(location.path_id >= paths.size()) {
                    return true;
                }
                auto stamp_it = stamps.find(location.path_id);
                auto it = hashes.find(location.path_id);
                if(dep_stale(to_ref(paths[location.path_id]),
                             stamp_it != stamps.end() ? std::optional(stamp_it->second)
                                                      : std::nullopt,
                             it != hashes.end() ? std::optional(it->second) : std::nullopt)) {
                    return true;
                }
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
        auto root = root_of(*self.buffer);
        auto paths = root[&Impl::paths];
        std::optional<std::uint32_t> local;
        for(std::uint32_t i = 0; i < paths.size(); ++i) {
            if(to_ref(paths[i]) == context_path) {
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
    self.rev = next_revision();
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
        auto found = root_of(*self.buffer)[&Impl::symbols].find(hash);
        if(found) {
            auto symbol = found->get<1>();
            name = std::string(symbol[&Symbol::name]);
            kind = SymbolKind(symbol[&Symbol::kind]);
            return true;
        }
    }
    return false;
}

void MergedIndex::merge_symbols(this Self& self, const SymbolTable& symbols) {
    self.rev = next_revision();
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
    self.rev = next_revision();
    self.load_in_memory();
    self.impl->content = content.str();
    self.impl->line_starts = kota::ipc::lsp::build_line_starts(self.impl->content);

    // Intern the dependencies into the shard's own path table. The staleness
    // baseline per distinct dep is two-part: the consumed-content hash the
    // worker computed from the compiler's own buffers (describing exactly
    // what the rows were built from), and a stat fast path recorded only for
    // files that provably did not change since before the build started —
    // for the rest the stat could describe content the rows were never built
    // from, so they re-earn their fast path through a hash check instead.
    std::vector<IncludeLocation> include_locations;
    llvm::SmallVector<DepHash> dep_hashes;
    llvm::SmallVector<DepStamp> dep_stamps;
    llvm::DenseSet<std::uint32_t> seen;
    include_locations.reserve(deps.size());
    auto baseline_before_ns = fs::stat_baseline_before_ns(build_at.count());
    for(auto& dep: deps) {
        auto local_id = self.impl->paths.path_id(dep.path);
        include_locations.push_back({local_id, dep.line, dep.include_id});
        if(!seen.insert(local_id).second) {
            continue;
        }

        fs::file_status status;
        bool stat_ok = !fs::status(dep.path, status);
        bool untouched = stat_ok && fs::mtime_ns(status) <= baseline_before_ns;

        auto hash = dep.content_hash;
        if(hash == 0 && untouched) {
            // The worker had no buffer to hash (e.g. behind a PCH); the
            // unchanged mtime proves the disk still holds the consumed
            // bytes, so hash it here.
            hash = hash_file(dep.path);
        }
        if(hash != 0) {
            dep_hashes.emplace_back(local_id, hash);
        }
        if(untouched) {
            dep_stamps.push_back({local_id, status.getSize(), fs::mtime_ns(status)});
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
        context.dep_stamps = std::move(dep_stamps);
    });
    self.impl->occurrences_cache.clear();
}

void MergedIndex::merge(this Self& self,
                        llvm::StringRef tu_path,
                        std::uint32_t include_id,
                        FileIndex& index,
                        llvm::StringRef content) {
    self.rev = next_revision();
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
        return to_ref(root_of(*self.buffer)[&Impl::content]);
    }
    return {};
}

std::span<const std::uint32_t> MergedIndex::line_starts(this const Self& self) {
    if(self.impl) {
        return self.impl->line_starts;
    } else if(self.buffer) {
        auto starts = to_array_ref(root_of(*self.buffer)[&Impl::line_starts]);
        return {starts.data(), starts.size()};
    }
    return {};
}

bool operator==(MergedIndex& lhs, MergedIndex& rhs) {
    lhs.load_in_memory();
    rhs.load_in_memory();
    return *lhs.impl == *rhs.impl;
}

}  // namespace clice::index

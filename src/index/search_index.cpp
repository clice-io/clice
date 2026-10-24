module;

#include "support/prelude.h"

#include "support/logging.macros.h"

module clice;

import :index.columns;
import :index.format;
import :index.search_index;
import :support.logging;

namespace clice::index {

namespace {

constexpr std::uint32_t no_entry = ~0u;

/// Deeper parent chains than this only come from corrupt bytes.
constexpr std::uint32_t max_chain = 64;

using Mode = SymbolQuery::Mode;

using Root = codec::blob::view_t<SearchIndexBlob>;

void append_postings(std::vector<std::uint8_t>& arena,
                     std::vector<std::uint32_t>& ends,
                     const auto& entries,
                     std::uint32_t count) {
    auto list = write_postings(entries, count);
    arena.insert(arena.end(), list.begin(), list.end());
    ends.push_back(static_cast<std::uint32_t>(arena.size()));
}

bool reserved_name(llvm::StringRef name) {
    return name.size() > 1 && name[0] == '_' && (name[1] == '_' || llvm::isUpper(name[1]));
}

/// The hits held while a search runs: the worst on top of the heap, so
/// a full set drops it for a better arrival.
struct TopHits {
    struct Ranked {
        NameRank rank;
        std::uint32_t entry;
    };

    std::uint32_t limit;
    llvm::function_ref<llvm::StringRef(std::uint32_t)> name_of;
    llvm::function_ref<llvm::StringRef(std::uint32_t)> args_of;
    llvm::function_ref<SymbolHash(std::uint32_t)> hash_of;
    std::vector<Ranked> hits;

    /// Whether a hit was turned away or evicted: the ranking runs past
    /// the limit.
    bool overflowed = false;

    bool before(const NameRank& lhs,
                llvm::StringRef lhs_name,
                llvm::StringRef lhs_args,
                SymbolHash lhs_hash,
                const Ranked& rhs) const {
        return ranks_after(rhs.rank,
                           name_of(rhs.entry),
                           args_of(rhs.entry),
                           hash_of(rhs.entry),
                           lhs,
                           lhs_name,
                           lhs_args,
                           lhs_hash);
    }

    bool before(const Ranked& lhs, const Ranked& rhs) const {
        return before(lhs.rank, name_of(lhs.entry), args_of(lhs.entry), hash_of(lhs.entry), rhs);
    }

    bool full() const {
        return hits.size() >= limit;
    }

    /// Whether a hit of `rank` could still enter, whatever its name.
    bool admits(const NameRank& rank) const {
        return !full() || before(rank, "", "", 0, hits.front());
    }

    void push(Ranked hit) {
        auto compare = [&](const Ranked& lhs, const Ranked& rhs) {
            return before(lhs, rhs);
        };
        if(!full()) {
            hits.push_back(hit);
            ranges::push_heap(hits, compare);
            return;
        }
        overflowed = true;
        if(!before(hit, hits.front())) {
            return;
        }
        ranges::pop_heap(hits, compare);
        hits.back() = hit;
        ranges::push_heap(hits, compare);
    }

    std::vector<Ranked> sorted() && {
        ranges::sort(hits, [&](const Ranked& lhs, const Ranked& rhs) { return before(lhs, rhs); });
        return std::move(hits);
    }
};

}  // namespace

bool is_searchable_kind(SymbolKind kind) {
    switch(kind) {
        case SymbolKind::Namespace:
        case SymbolKind::Class:
        case SymbolKind::Struct:
        case SymbolKind::Union:
        case SymbolKind::Enum:
        case SymbolKind::Type:
        case SymbolKind::Field:
        case SymbolKind::EnumMember:
        case SymbolKind::Function:
        case SymbolKind::Method:
        case SymbolKind::Variable:
        case SymbolKind::Parameter:
        case SymbolKind::Macro:
        case SymbolKind::Concept:
        case SymbolKind::Module:
        case SymbolKind::Operator:
        case SymbolKind::Attribute: return true;
        default: return false;
    }
}

float symbol_quality(llvm::StringRef name,
                     SymbolKind kind,
                     SymbolFlags flags,
                     std::uint32_t reference_files) {
    float quality =
        1 + std::min(1.0f, std::log10(static_cast<float>(std::max(reference_files, 1u))) / 3);
    switch(kind) {
        case SymbolKind::Namespace: quality *= 0.8f; break;
        case SymbolKind::Macro: quality *= 0.6f; break;
        case SymbolKind::Operator: quality *= 0.5f; break;
        default: break;
    }
    switch(name_form(flags)) {
        case NameForm::Constructor:
        case NameForm::Destructor:
        case NameForm::Conversion: quality *= 0.5f; break;
        default: break;
    }
    if(has_flag(flags, SymbolFlags::Deprecated) || reserved_name(name)) {
        quality *= 0.3f;
    }
    if(has_flag(flags, SymbolFlags::SpelledInMacro)) {
        quality *= 0.5f;
    }
    if(has_flag(flags, SymbolFlags::SystemHeader)) {
        quality *= 0.7f;
    }
    if(!has_flag(flags, SymbolFlags::HasDefinition)) {
        quality *= 0.8f;
    }
    return quality;
}

std::vector<std::byte> build_search_blob(const SearchSnapshot& snapshot) {
    struct Ranked {
        std::uint32_t entry;
        float quality;
    };

    std::vector<Ranked> ranked;
    llvm::DenseSet<SymbolHash> seen;
    for(std::uint32_t i = 0; i < snapshot.entries.size(); i += 1) {
        auto& entry = snapshot.entries[i];
        if(!searchable(entry.kind, entry.name, entry.flags) || entry.hash == 0 ||
           reserved_key(entry.hash) || !seen.insert(entry.hash).second) {
            continue;
        }
        ranked.push_back(
            {i, symbol_quality(entry.name, entry.kind, entry.flags, entry.reference_files)});
    }
    ranges::sort(ranked, [&](const Ranked& lhs, const Ranked& rhs) {
        if(lhs.quality != rhs.quality) {
            return lhs.quality > rhs.quality;
        }
        return snapshot.entries[lhs.entry].hash < snapshot.entries[rhs.entry].hash;
    });
    auto count = static_cast<std::uint32_t>(ranked.size());
    auto entry_of = [&](std::uint32_t id) -> const SearchEntry& {
        return snapshot.entries[ranked[id].entry];
    };

    llvm::DenseMap<SymbolHash, std::uint32_t> id_of;
    id_of.reserve(count);
    for(std::uint32_t id = 0; id < count; id += 1) {
        id_of.try_emplace(entry_of(id).hash, id);
    }

    struct Scope {
        SymbolFlags flags;
        SymbolHash parent;
    };

    // The chain a qualified name spells skips transparent scopes: inline
    // namespaces are documents, anonymous scopes are not, so their parents
    // are kept apart.
    llvm::DenseMap<SymbolHash, SymbolHash> anonymous_parents;
    for(auto& entry: snapshot.entries) {
        if(has_flag(entry.flags, SymbolFlags::AnonymousScope)) {
            anonymous_parents.try_emplace(entry.hash, entry.parent);
        }
    }
    auto scope_of = [&](SymbolHash hash) -> std::optional<Scope> {
        if(auto anonymous = anonymous_parents.find(hash); anonymous != anonymous_parents.end()) {
            return Scope{SymbolFlags::AnonymousScope, anonymous->second};
        }
        auto it = id_of.find(hash);
        if(it == id_of.end()) {
            return std::nullopt;
        }
        auto& entry = entry_of(it->second);
        return Scope{entry.flags, entry.parent};
    };
    auto parent_of = [&](std::uint32_t id) {
        auto parent = no_entry;
        auto& entry = entry_of(id);
        for_each_spelled_scope(entry.hash, entry.parent, scope_of, [&](SymbolHash scope, Scope&) {
            parent = id_of.find(scope)->second;
            return false;
        });
        return parent;
    };

    SearchIndexBlob blob;
    blob.format_version = index_format_version;
    blob.search_version = search_format_version;
    blob.generation = snapshot.generation;
    blob.hashes.reserve(count);
    blob.name_ends.reserve(count);
    blob.args_ends.reserve(count);
    blob.parents.reserve(count);
    blob.qualities.reserve(count);
    std::vector<std::string> lower_names;
    lower_names.reserve(count);
    for(std::uint32_t id = 0; id < count; id += 1) {
        auto& entry = entry_of(id);
        if(entry.local) {
            assert(entry.file != no_file && "an entry of internal linkage names its file");
            blob.local_entries.push_back(id);
            blob.local_paths.push_back(entry.file);
        }
        blob.hashes.push_back(entry.hash);
        blob.names += entry.name;
        blob.name_ends.push_back(static_cast<std::uint32_t>(blob.names.size()));
        blob.args += entry.args;
        blob.args_ends.push_back(static_cast<std::uint32_t>(blob.args.size()));
        blob.parents.push_back(parent_of(id));
        blob.qualities.push_back(ranked[id].quality);
        lower_names.push_back(llvm::StringRef(entry.name).lower());
    }

    blob.by_lower_name.resize(count);
    std::iota(blob.by_lower_name.begin(), blob.by_lower_name.end(), 0u);
    ranges::sort(blob.by_lower_name, [&](std::uint32_t lhs, std::uint32_t rhs) {
        if(lower_names[lhs] != lower_names[rhs]) {
            return lower_names[lhs] < lower_names[rhs];
        }
        if(entry_of(lhs).name != entry_of(rhs).name) {
            return entry_of(lhs).name < entry_of(rhs).name;
        }
        return lhs < rhs;
    });
    blob.by_hash.resize(count);
    std::iota(blob.by_hash.begin(), blob.by_hash.end(), 0u);
    ranges::sort(blob.by_hash, [&](std::uint32_t lhs, std::uint32_t rhs) {
        return entry_of(lhs).hash < entry_of(rhs).hash;
    });

    // Every (token, id) pair, token-major, ordered by token with each
    // token's ids left ascending as they arrive: an LSD radix sort over the
    // token's three bytes, every pass stable.
    std::vector<std::uint64_t> pairs;
    llvm::SmallVector<NameToken, 64> tokens;
    for(std::uint32_t id = 0; id < count; id += 1) {
        name_tokens(entry_of(id).name, tokens);
        for(auto token: tokens) {
            pairs.push_back(std::uint64_t(token) << 32 | id);
        }
    }
    std::vector<std::uint64_t> sorted(pairs.size());
    for(unsigned shift = 32; shift < 56; shift += 8) {
        std::array<std::size_t, 257> starts{};
        for(auto pair: pairs) {
            starts[(pair >> shift & 0xff) + 1] += 1;
        }
        std::partial_sum(starts.begin(), starts.end(), starts.begin());
        for(auto pair: pairs) {
            auto& slot = starts[pair >> shift & 0xff];
            sorted[slot] = pair;
            slot += 1;
        }
        std::swap(pairs, sorted);
    }
    std::vector<std::uint32_t> ids;
    for(std::size_t at = 0; at < pairs.size();) {
        auto token = static_cast<NameToken>(pairs[at] >> 32);
        ids.clear();
        for(; at < pairs.size() && pairs[at] >> 32 == token; at += 1) {
            ids.push_back(static_cast<std::uint32_t>(pairs[at]));
        }
        blob.token_keys.push_back(token);
        append_postings(blob.token_postings, blob.token_ends, ids, count);
    }

    llvm::DenseMap<std::uint32_t, Bitmap> children;
    Bitmap top_level;
    for(std::uint32_t id = 0; id < count; id += 1) {
        auto parent = blob.parents[id];
        if(parent == no_entry) {
            top_level.add(id);
        } else {
            children[parent].add(id);
        }
    }
    for(auto container:
        views::transform(children, [](auto& entry) -> auto& { return entry.first; })) {
        blob.container_entries.push_back(container);
    }
    ranges::sort(blob.container_entries);
    for(auto container: blob.container_entries) {
        append_postings(blob.children_postings,
                        blob.children_ends,
                        children.find(container)->second,
                        count);
    }
    blob.top_level = write_postings(top_level, count);
    Bitmap long_names;
    for(std::uint32_t id = 0; id < count; id += 1) {
        if(entry_of(id).name.size() > name_bound) {
            long_names.add(id);
        }
    }
    blob.long_names = write_postings(long_names, count);

    llvm::DenseMap<std::uint32_t, Bitmap> subtrees;
    llvm::DenseSet<std::uint32_t> visiting;
    auto subtree_of = [&](this auto& self, std::uint32_t container) -> const Bitmap& {
        if(auto it = subtrees.find(container); it != subtrees.end()) {
            return it->second;
        }
        Bitmap below;
        // A container in its own subtree only comes from corrupt parent
        // links; it contributes nothing rather than recursing forever.
        if(visiting.insert(container).second) {
            below = children.find(container)->second;
            for(auto child: children.find(container)->second) {
                if(children.contains(child)) {
                    below |= self(child);
                }
            }
            visiting.erase(container);
        }
        return subtrees.try_emplace(container, std::move(below)).first->second;
    };
    for(auto container: blob.container_entries) {
        if(entry_of(container).kind == SymbolKind::Namespace) {
            blob.subtree_entries.push_back(container);
            append_postings(blob.subtree_postings, blob.subtree_ends, subtree_of(container), count);
        }
    }

    constexpr auto kind_count = kota::meta::reflection<SymbolKind::Kind>::member_names.size();
    std::vector<Bitmap> kinds(kind_count);
    std::vector<Bitmap> files(snapshot.paths.size());
    for(std::uint32_t id = 0; id < count; id += 1) {
        auto& entry = entry_of(id);
        if(entry.kind.value() < kind_count) {
            kinds[entry.kind.value()].add(id);
        }
        if(entry.file != no_file && entry.file < files.size()) {
            files[entry.file].add(id);
        }
    }
    for(auto& bitmap: kinds) {
        append_postings(blob.kind_postings, blob.kind_ends, bitmap, count);
    }
    blob.paths = snapshot.paths;
    for(auto& bitmap: files) {
        append_postings(blob.file_postings, blob.file_ends, bitmap, count);
    }

    return encode(blob);
}

bool ranks_after(const NameRank& lhs,
                 llvm::StringRef lhs_name,
                 llvm::StringRef lhs_args,
                 SymbolHash lhs_hash,
                 const NameRank& rhs,
                 llvm::StringRef rhs_name,
                 llvm::StringRef rhs_args,
                 SymbolHash rhs_hash) {
    if(lhs.tier != rhs.tier) {
        return lhs.tier > rhs.tier;
    }
    if(lhs.score != rhs.score) {
        return lhs.score < rhs.score;
    }
    if(lhs_name != rhs_name) {
        return lhs_name > rhs_name;
    }
    if(lhs_args != rhs_args) {
        return lhs_args > rhs_args;
    }
    return lhs_hash > rhs_hash;
}

NameRanker::NameRanker(const SymbolQuery& query) :
    query(query), lower_pattern(llvm::StringRef(query.pattern).lower()),
    exact(query.mode == Mode::Fuzzy ? llvm::StringRef(query.pattern) : llvm::StringRef(),
          MatchOptions{.inside_word = true}) {
    llvm::SmallVector<TypoAlternative> alternatives;
    if(query.mode == Mode::Fuzzy) {
        typo_tokens(query.pattern, alternatives);
        query_tokens(query.pattern, short_tokens);
        if(!short_tokens.empty() && short_tokens.front() >= first_trigram) {
            short_tokens.clear();
        }
    }
    if(!alternatives.empty()) {
        typo.emplace(query.pattern, MatchOptions{.typo = true, .inside_word = true});
    }
}

std::optional<NameRank>
    NameRanker::rank(llvm::StringRef name, llvm::StringRef args, float quality, bool lenient) {
    if(!args_match(query.args, args)) {
        return std::nullopt;
    }
    switch(query.mode) {
        case Mode::Members:
        case Mode::Subtree: return NameRank{.tier = 2, .score = 0};
        case Mode::Exact:
            if(name == query.pattern) {
                return NameRank{.tier = 0, .score = quality};
            }
            return std::nullopt;
        case Mode::Glob:
            if(glob_matches(query.pattern, name)) {
                return NameRank{.tier = 2, .score = quality};
            }
            return std::nullopt;
        case Mode::Fuzzy: break;
    }
    if(query.pattern.empty()) {
        return NameRank{.tier = 2, .score = quality};
    }
    if(name.size() == lower_pattern.size() && name.lower() == lower_pattern) {
        return NameRank{.tier = static_cast<std::uint8_t>(name == query.pattern ? 0 : 1),
                        .score = quality};
    }
    // A query too short for trigrams is keyed by a name's first two words
    // in the index; judging entries the same way keeps the two answers alike.
    if(!short_tokens.empty()) {
        llvm::SmallVector<NameToken, 64> tokens;
        name_tokens(name, tokens);
        if(!ranges::all_of(short_tokens,
                           [&](NameToken token) { return ranges::binary_search(tokens, token); })) {
            return std::nullopt;
        }
    }
    if(auto score = exact.match(name)) {
        return NameRank{.tier = 2, .score = *score * quality};
    }
    if(lenient && typo) {
        if(auto score = typo->match(name)) {
            return NameRank{.tier = 3, .score = *score * quality};
        }
    }
    return std::nullopt;
}

/// The mapped blob with its columns bound and its posting lists decoded
/// on first use.
struct SearchIndex::View {
    using Blob = SearchIndexBlob;

    std::unique_ptr<llvm::MemoryBuffer> buffer;

    const codec::blob::view_t<Blob>* columns = nullptr;

    mutable llvm::DenseMap<std::uint32_t, Bitmap> token_cache;
    mutable llvm::DenseMap<std::uint32_t, Bitmap> children_cache;
    mutable llvm::DenseMap<std::uint32_t, Bitmap> subtree_cache;
    mutable llvm::DenseMap<std::uint32_t, Bitmap> kind_cache;
    mutable llvm::DenseMap<std::uint32_t, Bitmap> file_cache;
    mutable std::optional<Bitmap> top_level_cache;
    mutable std::optional<Bitmap> long_names_cache;

    /// Whether a posting list failed to decode: the answers since are
    /// incomplete, and the owner rebuilds the index.
    mutable bool damaged = false;

    /// Bind the columns of a verified blob, checking they line up.
    std::expected<void, llvm::StringRef> bind(const Root& root);

    std::uint32_t count() const {
        return static_cast<std::uint32_t>(columns->hashes.size());
    }

    llvm::StringRef name(std::uint32_t entry) const {
        return slice(columns->names, columns->name_ends, entry);
    }

    llvm::StringRef arguments(std::uint32_t entry) const {
        return slice(columns->args, columns->args_ends, entry);
    }

    /// The portable name of the file an entry of internal linkage lives in;
    /// empty for any other entry.
    llvm::StringRef local_file(std::uint32_t entry) const {
        auto position = sorted_find(columns->local_entries, entry);
        if(!position) {
            return {};
        }
        return llvm::StringRef(columns->paths[columns->local_paths[*position]]);
    }

    /// A posting list decoded; a malformed one reads as empty and marks
    /// the index damaged.
    Bitmap decode(llvm::ArrayRef<std::uint8_t> arena,
                  llvm::ArrayRef<std::uint32_t> ends,
                  std::uint32_t i) const {
        auto decoded = read_postings(slice(arena, ends, i), count());
        if(!decoded) {
            if(!damaged) {
                LOG_WARN("A search index posting list does not decode; the index is rebuilt");
            }
            damaged = true;
            return {};
        }
        return std::move(*decoded);
    }

    const Bitmap& cached(llvm::DenseMap<std::uint32_t, Bitmap>& cache,
                         llvm::ArrayRef<std::uint8_t> arena,
                         llvm::ArrayRef<std::uint32_t> ends,
                         std::uint32_t i) const {
        auto [it, inserted] = cache.try_emplace(i);
        if(inserted) {
            it->second = decode(arena, ends, i);
        }
        return it->second;
    }

    /// The entries carrying a token, null when none does.
    const Bitmap* token(NameToken key) const {
        auto i = sorted_find(columns->token_keys, key);
        if(!i) {
            return nullptr;
        }
        return &cached(token_cache, columns->token_postings, columns->token_ends, *i);
    }

    /// The direct members of a container, the top level for no_entry.
    const Bitmap& children(std::uint32_t container) const {
        if(container == no_entry) {
            if(!top_level_cache) {
                std::uint32_t ends[] = {static_cast<std::uint32_t>(columns->top_level.size())};
                top_level_cache = decode(columns->top_level, ends, 0);
            }
            return *top_level_cache;
        }
        auto i = sorted_find(columns->container_entries, container);
        if(!i) {
            const static Bitmap none;
            return none;
        }
        return cached(children_cache, columns->children_postings, columns->children_ends, *i);
    }

    /// The entries whose name outruns the token bound.
    const Bitmap& long_names() const {
        if(!long_names_cache) {
            std::uint32_t ends[] = {static_cast<std::uint32_t>(columns->long_names.size())};
            long_names_cache = decode(columns->long_names, ends, 0);
        }
        return *long_names_cache;
    }

    bool is_container(std::uint32_t entry) const {
        return ranges::binary_search(columns->container_entries, entry);
    }

    /// Everything below a container: stored for namespaces, everything
    /// for the top level, gathered from the members of other containers.
    Bitmap subtree(std::uint32_t container) const {
        if(container == no_entry) {
            Bitmap all;
            all.addRange(0, count());
            return all;
        }
        if(auto i = sorted_find(columns->subtree_entries, container)) {
            return cached(subtree_cache, columns->subtree_postings, columns->subtree_ends, *i);
        }
        Bitmap below = children(container);
        llvm::SmallVector<std::uint32_t> pending(below.begin(), below.end());
        llvm::DenseSet<std::uint32_t> visited{container};
        while(!pending.empty()) {
            auto entry = pending.pop_back_val();
            if(!visited.insert(entry).second || !is_container(entry)) {
                continue;
            }
            for(auto member: children(entry)) {
                below.add(member);
                pending.push_back(member);
            }
        }
        return below;
    }

    const Bitmap& kind(SymbolKind kind) const {
        auto value = static_cast<std::uint32_t>(kind.value());
        if(value >= columns->kind_ends.size()) {
            const static Bitmap none;
            return none;
        }
        return cached(kind_cache, columns->kind_postings, columns->kind_ends, value);
    }

    const Bitmap& file(std::uint32_t index) const {
        return cached(file_cache, columns->file_postings, columns->file_ends, index);
    }

    /// The chain of containers above an entry, outermost first.
    llvm::SmallVector<ScopeEntry, 8> chain(std::uint32_t entry) const {
        llvm::SmallVector<ScopeEntry, 8> entries;
        auto parent = columns->parents[entry];
        for(std::uint32_t depth = 0; parent != no_entry && depth < max_chain; depth += 1) {
            entries.push_back({.name = name(parent), .args = arguments(parent)});
            parent = columns->parents[parent];
        }
        ranges::reverse(entries);
        return entries;
    }

    /// The entries whose lowercase name is `lower`, in by_lower_name order.
    llvm::ArrayRef<std::uint32_t> lower_range(llvm::StringRef lower) const {
        auto project = [&](std::uint32_t entry) {
            return name(entry).lower();
        };
        auto& entries = columns->by_lower_name;
        auto range = ranges::equal_range(entries, lower.str(), {}, project);
        return {range.begin(), range.end()};
    }

    /// Every posting list of `tokens` intersected, none when a token has
    /// no postings at all.
    std::optional<Bitmap> intersection(llvm::ArrayRef<NameToken> tokens) const {
        llvm::SmallVector<const Bitmap*, 16> lists;
        for(auto key: tokens) {
            auto* list = token(key);
            if(!list) {
                return std::nullopt;
            }
            lists.push_back(list);
        }
        // From the rarest on: each intersection is bounded by the
        // smallest operand so far.
        ranges::sort(lists, [](const Bitmap* lhs, const Bitmap* rhs) {
            return lhs->cardinality() < rhs->cardinality();
        });
        Bitmap result = *lists.front();
        for(auto* list: views::drop(lists, 1)) {
            result &= *list;
            if(result.isEmpty()) {
                break;
            }
        }
        return result;
    }
};

std::expected<void, llvm::StringRef> SearchIndex::View::bind(const Root& root) {
    auto& view = *this;
    if(root.search_version != search_format_version) {
        return std::unexpected("written by another search format version");
    }
    view.columns = &root;
    auto& columns = view.columns;
    llvm::ArrayRef<std::uint64_t> hashes = columns->hashes;
    llvm::ArrayRef<std::uint32_t> by_hash = columns->by_hash;

    auto count = hashes.size();
    if(columns->name_ends.size() != count || columns->args_ends.size() != count ||
       columns->parents.size() != count || columns->qualities.size() != count ||
       columns->by_lower_name.size() != count || by_hash.size() != count) {
        return std::unexpected("entry columns do not line up");
    }
    if(!monotone_ends(columns->name_ends, columns->names.size()) ||
       !monotone_ends(columns->args_ends, columns->args.size())) {
        return std::unexpected("name arenas do not line up");
    }
    for(auto parent: columns->parents) {
        if(parent != no_entry && parent >= count) {
            return std::unexpected("parent outside the entries");
        }
    }
    // FIXME: by_lower_name is range-checked but not checked for its order: a
    // blob out of lowercase-name order loads, and lower_range's binary search
    // misses entries silently.
    for(auto entry: columns->by_lower_name) {
        if(entry >= count) {
            return std::unexpected("name order outside the entries");
        }
    }
    for(std::uint32_t i = 0; i < count; i += 1) {
        auto entry = by_hash[i];
        if(entry >= count || (i > 0 && hashes[entry] <= hashes[by_hash[i - 1]])) {
            return std::unexpected("hash order is not ascending");
        }
    }
    // Hashes become table keys downstream (symbol_info), which reserve
    // two sentinel values.
    for(auto hash: hashes) {
        if(reserved_key(hash)) {
            return std::unexpected("reserved symbol hash");
        }
    }
    if(columns->token_keys.size() != columns->token_ends.size() ||
       !monotone_ends(columns->token_ends, columns->token_postings.size()) ||
       !strictly_ascending(columns->token_keys)) {
        return std::unexpected("token columns do not line up");
    }
    if(!sparse_ok(columns->container_entries, columns->children_ends.size(), count) ||
       !monotone_ends(columns->children_ends, columns->children_postings.size())) {
        return std::unexpected("container columns do not line up");
    }
    if(!sparse_ok(columns->subtree_entries, columns->subtree_ends.size(), count) ||
       !monotone_ends(columns->subtree_ends, columns->subtree_postings.size())) {
        return std::unexpected("subtree columns do not line up");
    }
    if(!monotone_ends(columns->kind_ends, columns->kind_postings.size())) {
        return std::unexpected("kind columns do not line up");
    }
    if(columns->paths.size() != columns->file_ends.size() ||
       !monotone_ends(columns->file_ends, columns->file_postings.size())) {
        return std::unexpected("file columns do not line up");
    }
    if(!sparse_ok(columns->local_entries, columns->local_paths.size(), count) ||
       ranges::any_of(columns->local_paths,
                      [&](std::uint32_t path) { return path >= columns->paths.size(); })) {
        return std::unexpected("local columns do not line up");
    }
    return {};
}

SearchIndex::SearchIndex() = default;
SearchIndex::~SearchIndex() = default;
SearchIndex::SearchIndex(SearchIndex&&) noexcept = default;
SearchIndex& SearchIndex::operator=(SearchIndex&&) noexcept = default;

bool SearchIndex::load(std::unique_ptr<llvm::MemoryBuffer> blob) {
    view.reset();
    if(!blob) {
        return false;
    }
    blob = in_place_buffer(std::move(blob));
    auto root = verify_root<SearchIndexBlob>(blob->getBuffer(), "search blob");
    if(!root) {
        return false;
    }
    auto bound = std::make_unique<View>();
    if(auto ok = bound->bind(*root); !ok) {
        LOG_DEBUG("Rejecting search blob: {}", std::string_view(ok.error()));
        return false;
    }
    bound->buffer = std::move(blob);
    view = std::move(bound);
    return true;
}

bool SearchIndex::loaded() const {
    return view != nullptr;
}

std::uint32_t SearchIndex::size() const {
    return view ? view->count() : 0;
}

std::uint64_t SearchIndex::generation() const {
    return view ? view->columns->generation : 0;
}

bool SearchIndex::damaged() const {
    return view && view->damaged;
}

bool SearchIndex::contains(SymbolHash hash) const {
    if(!view) {
        return false;
    }
    return ranges::binary_search(view->columns->by_hash, hash, {}, [&](std::uint32_t entry) {
        return view->columns->hashes[entry];
    });
}

SearchOutcome SearchIndex::search(const SymbolQuery& query,
                                  std::uint32_t limit,
                                  CanonicalRef workspace) const {
    if(!view || limit == 0 || !query.by_pattern()) {
        return {};
    }
    auto& index = *view;

    // Everything a hit must satisfy besides its name: the kinds, the
    // files and the scope, as one bitmap; none when nothing narrows.
    std::optional<Bitmap> filter;
    auto narrow = [&](Bitmap allowed) {
        if(filter) {
            *filter &= allowed;
        } else {
            filter = std::move(allowed);
        }
    };
    if(!query.kinds.empty()) {
        Bitmap allowed;
        for(auto kind: query.kinds) {
            allowed |= index.kind(kind);
        }
        narrow(std::move(allowed));
    }
    if(!query.paths.empty()) {
        Bitmap allowed;
        for(std::uint32_t i = 0; i < index.columns->paths.size(); i += 1) {
            llvm::SmallString<256> buffer;
            auto file = path::local(llvm::StringRef(index.columns->paths[i]), workspace, buffer);
            if(ranges::any_of(query.paths, [&](const std::string& wanted) {
                   return path_matches(wanted, file);
               })) {
                allowed |= index.file(i);
            }
        }
        narrow(std::move(allowed));
    }
    if(query.external_only && !index.columns->local_entries.empty()) {
        Bitmap allowed;
        allowed.addRange(0, index.count());
        for(auto entry: index.columns->local_entries) {
            allowed.remove(entry);
        }
        narrow(std::move(allowed));
    }
    if(query.absolute || !query.scope.empty()) {
        // The containers the scope leads to: the top level for an empty
        // absolute scope, else every container named like the last
        // segment whose own chain the earlier segments describe.
        llvm::SmallVector<std::uint32_t, 4> containers;
        if(query.scope.empty()) {
            containers.push_back(no_entry);
        } else {
            auto& last = query.scope.back();
            llvm::ArrayRef<SymbolQuery::Segment> above = query.scope;
            above = above.drop_back();
            for(auto entry: index.lower_range(llvm::StringRef(last.name).lower())) {
                if(index.is_container(entry) && args_match(last.args, index.arguments(entry)) &&
                   segments_match(above, index.chain(entry), query.absolute)) {
                    containers.push_back(entry);
                }
            }
        }
        Bitmap allowed;
        for(auto container: containers) {
            allowed |= query.direct() ? index.children(container) : index.subtree(container);
        }
        narrow(std::move(allowed));
    }
    if(filter && filter->isEmpty()) {
        return {};
    }
    auto allowed = [&](std::uint32_t entry) {
        return !filter || filter->contains(entry);
    };

    NameRanker ranker(query);
    auto name_of = [&](std::uint32_t entry) {
        return index.name(entry);
    };
    auto args_of = [&](std::uint32_t entry) {
        return index.arguments(entry);
    };
    auto hash_of = [&](std::uint32_t entry) {
        return index.columns->hashes[entry];
    };
    TopHits top{.limit = limit, .name_of = name_of, .args_of = args_of, .hash_of = hash_of};
    // The entries already ranked; an entry the clean pass rejected is judged
    // again by the typo pass, which may accept it.
    Bitmap ranked;
    auto consider = [&](std::uint32_t entry, bool lenient) {
        if(auto rank = ranker.rank(index.name(entry),
                                   index.arguments(entry),
                                   index.columns->qualities[entry],
                                   lenient)) {
            ranked.add(entry);
            top.push({.rank = *rank, .entry = entry});
        }
    };
    // Entries come in quality order, and no name score exceeds 1 past the
    // exact tier, so once an entry's quality cannot enter the held set, no
    // later entry can either.
    auto scan = [&](const Bitmap& candidates, bool lenient) {
        for(auto entry: candidates) {
            if(entry >= index.count()) {
                break;
            }
            if(!allowed(entry) || ranked.contains(entry)) {
                continue;
            }
            if(!top.admits({.tier = static_cast<std::uint8_t>(lenient ? 3 : 2),
                            .score = index.columns->qualities[entry]})) {
                top.overflowed = true;
                break;
            }
            consider(entry, lenient);
        }
    };
    auto everything = [&] {
        if(filter) {
            return *filter;
        }
        Bitmap all;
        all.addRange(0, index.count());
        return all;
    };

    switch(query.mode) {
        case Mode::Members:
        case Mode::Subtree: scan(everything(), false); break;
        case Mode::Exact:
            for(auto entry: index.lower_range(llvm::StringRef(query.pattern).lower())) {
                if(allowed(entry)) {
                    consider(entry, false);
                }
            }
            break;
        case Mode::Glob: {
            llvm::SmallVector<NameToken, 16> tokens;
            for(auto literal: glob_literals(query.pattern)) {
                llvm::SmallVector<NameToken, 8> piece;
                query_tokens(literal, piece);
                // A literal too short for trigrams gives the short form,
                // which keys the first words only: no constraint from it.
                if(!piece.empty() && piece.front() >= first_trigram) {
                    tokens.append(piece);
                }
            }
            if(tokens.empty()) {
                scan(everything(), false);
                break;
            }
            ranges::sort(tokens);
            tokens.erase(ranges::unique(tokens).begin(), tokens.end());
            scan(index.intersection(tokens).value_or(Bitmap{}) | index.long_names(), false);
            break;
        }
        case Mode::Fuzzy: {
            if(query.pattern.empty()) {
                scan(everything(), false);
                break;
            }
            for(auto entry: index.lower_range(llvm::StringRef(query.pattern).lower())) {
                if(allowed(entry)) {
                    consider(entry, false);
                }
            }
            llvm::SmallVector<NameToken, 16> tokens;
            query_tokens(query.pattern, tokens);
            if(tokens.empty()) {
                scan(everything(), false);
                break;
            }
            scan(index.intersection(tokens).value_or(Bitmap{}) | index.long_names(), false);
            if(top.full() || !ranker.has_typo_matches()) {
                break;
            }
            llvm::SmallVector<TypoAlternative> alternatives;
            typo_tokens(query.pattern, alternatives);
            Bitmap candidates = index.long_names();
            for(auto& alternative: alternatives) {
                if(auto some = index.intersection(alternative.tokens)) {
                    candidates |= *some;
                }
            }
            scan(candidates, true);
            break;
        }
    }

    SearchOutcome outcome{.exhausted = !top.overflowed};
    for(auto& ranked: std::move(top).sorted()) {
        outcome.hits.push_back({.hash = index.columns->hashes[ranked.entry],
                                .rank = ranked.rank,
                                .file = index.local_file(ranked.entry)});
    }
    return outcome;
}

}  // namespace clice::index

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

/// A symbol the builder places: one the run offers, or a scope above those.
struct Node {
    const SearchEntry* symbol = nullptr;
    float quality = 0;
    bool offered = false;
    /// The node of the nearest enclosing symbol a qualified name spells,
    /// or no_entry.
    std::uint32_t scope = no_entry;
};

/// The hits held while a search runs: the worst on top of the heap, so
/// a full set drops it for a better arrival.
struct TopHits {
    struct Ranked {
        NameRank rank;
        llvm::StringRef name;
        llvm::StringRef args;
        SymbolHash hash = 0;
        std::uint32_t file = no_file;
    };

    std::uint32_t limit;
    std::vector<Ranked> hits;

    /// Whether a hit was turned away or evicted: the ranking runs past
    /// the limit.
    bool overflowed = false;

    static bool before(const Ranked& lhs, const Ranked& rhs) {
        return ranks_after(rhs.rank,
                           rhs.name,
                           rhs.args,
                           rhs.hash,
                           lhs.rank,
                           lhs.name,
                           lhs.args,
                           lhs.hash);
    }

    bool full() const {
        return hits.size() >= limit;
    }

    /// Whether a hit of `rank` could still enter, whatever its name.
    bool admits(const NameRank& rank) const {
        return !full() || before({.rank = rank}, hits.front());
    }

    void push(Ranked hit) {
        if(!full()) {
            hits.push_back(hit);
            ranges::push_heap(hits, before);
            return;
        }
        overflowed = true;
        if(!before(hit, hits.front())) {
            return;
        }
        ranges::pop_heap(hits, before);
        hits.back() = hit;
        ranges::push_heap(hits, before);
    }

    std::vector<Ranked> sorted() && {
        ranges::sort(hits, before);
        return std::move(hits);
    }
};

/// Encode one segment offering `offered`, the nodes of a slice of the
/// run, with the scopes above them it does not hold.
std::vector<std::byte> encode_segment(llvm::ArrayRef<Node> nodes,
                                      llvm::ArrayRef<std::uint32_t> offered,
                                      llvm::ArrayRef<std::uint32_t> covered) {
    std::vector<std::uint32_t> members(offered.begin(), offered.end());
    ranges::sort(members, [&](std::uint32_t lhs, std::uint32_t rhs) {
        if(nodes[lhs].quality != nodes[rhs].quality) {
            return nodes[lhs].quality > nodes[rhs].quality;
        }
        return nodes[lhs].symbol->hash < nodes[rhs].symbol->hash;
    });
    auto count = static_cast<std::uint32_t>(members.size());
    llvm::DenseMap<std::uint32_t, std::uint32_t> id_of;
    id_of.reserve(count);
    for(std::uint32_t id = 0; id < count; id += 1) {
        id_of.try_emplace(members[id], id);
    }
    for(std::uint32_t id = 0; id < count; id += 1) {
        for(auto at = nodes[members[id]].scope;
            at != no_entry &&
            id_of.try_emplace(at, static_cast<std::uint32_t>(members.size())).second;
            at = nodes[at].scope) {
            members.push_back(at);
        }
    }
    auto total = static_cast<std::uint32_t>(members.size());
    auto entry_of = [&](std::uint32_t id) -> const SearchEntry& {
        return *nodes[members[id]].symbol;
    };

    SearchIndexBlob blob;
    blob.format_version = index_format_version;
    blob.search_version = search_format_version;
    blob.hashes.reserve(total);
    blob.name_ends.reserve(total);
    blob.args_ends.reserve(total);
    blob.parents.reserve(total);
    blob.qualities.reserve(count);
    std::vector<std::string> lower_names;
    lower_names.reserve(total);
    for(std::uint32_t id = 0; id < total; id += 1) {
        auto& entry = entry_of(id);
        if(id < count) {
            if(entry.local) {
                assert(entry.file != no_file && "an entry of internal linkage names its file");
                blob.local_entries.push_back(id);
                blob.local_paths.push_back(entry.file);
            }
            blob.qualities.push_back(nodes[members[id]].quality);
        }
        blob.hashes.push_back(entry.hash);
        blob.names += entry.name;
        blob.name_ends.push_back(static_cast<std::uint32_t>(blob.names.size()));
        blob.args += entry.args;
        blob.args_ends.push_back(static_cast<std::uint32_t>(blob.args.size()));
        auto scope = nodes[members[id]].scope;
        blob.parents.push_back(scope == no_entry ? no_entry : id_of.find(scope)->second);
        lower_names.push_back(llvm::StringRef(entry.name).lower());
    }

    blob.by_lower_name.resize(total);
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
            auto& next = starts[pair >> shift & 0xff];
            sorted[next] = pair;
            next += 1;
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
    for(std::uint32_t id = 0; id < total; id += 1) {
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
                        total);
    }
    blob.top_level = write_postings(top_level, total);
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
            append_postings(blob.subtree_postings, blob.subtree_ends, subtree_of(container), total);
        }
    }

    constexpr auto kind_count = kota::meta::reflection<SymbolKind::Kind>::member_names.size();
    std::vector<Bitmap> kinds(kind_count);
    llvm::DenseMap<std::uint32_t, Bitmap> files;
    for(std::uint32_t id = 0; id < count; id += 1) {
        auto& entry = entry_of(id);
        if(entry.kind.value() < kind_count) {
            kinds[entry.kind.value()].add(id);
        }
        if(entry.file != no_file) {
            files[entry.file].add(id);
        }
    }
    for(auto& bitmap: kinds) {
        append_postings(blob.kind_postings, blob.kind_ends, bitmap, count);
    }
    for(auto& [path, entries]: files) {
        blob.paths.push_back(path);
    }
    ranges::sort(blob.paths);
    for(auto path: blob.paths) {
        append_postings(blob.file_postings, blob.file_ends, files.find(path)->second, count);
    }
    blob.covered.assign(covered.begin(), covered.end());

    return encode(blob);
}

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

std::vector<std::vector<std::byte>> build_search_segments(const SearchSnapshot& snapshot,
                                                          std::uint32_t bound) {
    std::vector<Node> nodes;
    llvm::DenseMap<SymbolHash, std::uint32_t> node_of;
    auto place = [&](const SearchEntry& entry, bool offered) {
        if(!searchable(entry.kind, entry.name, entry.flags) || entry.hash == 0 ||
           reserved_key(entry.hash) ||
           !node_of.try_emplace(entry.hash, static_cast<std::uint32_t>(nodes.size())).second) {
            return;
        }
        nodes.push_back({
            .symbol = &entry,
            .quality =
                offered ? symbol_quality(entry.name, entry.kind, entry.flags, entry.reference_files)
                        : 0,
            .offered = offered,
        });
    };
    for(auto& entry: snapshot.entries) {
        place(entry, true);
    }
    for(auto& scope: snapshot.scopes) {
        place(scope, false);
    }

    struct Scope {
        SymbolFlags flags;
        SymbolHash parent;
    };

    // The chain a qualified name spells skips transparent scopes: inline
    // namespaces are documents, anonymous scopes are not, so their parents
    // are kept apart.
    llvm::DenseMap<SymbolHash, SymbolHash> anonymous_parents;
    for(auto* symbols: {&snapshot.entries, &snapshot.scopes}) {
        for(auto& entry: *symbols) {
            if(has_flag(entry.flags, SymbolFlags::AnonymousScope)) {
                anonymous_parents.try_emplace(entry.hash, entry.parent);
            }
        }
    }
    auto scope_of = [&](SymbolHash hash) -> std::optional<Scope> {
        if(auto anonymous = anonymous_parents.find(hash); anonymous != anonymous_parents.end()) {
            return Scope{SymbolFlags::AnonymousScope, anonymous->second};
        }
        auto it = node_of.find(hash);
        if(it == node_of.end()) {
            return std::nullopt;
        }
        auto& symbol = *nodes[it->second].symbol;
        return Scope{symbol.flags, symbol.parent};
    };
    for(auto& node: nodes) {
        for_each_spelled_scope(node.symbol->hash,
                               node.symbol->parent,
                               scope_of,
                               [&](SymbolHash scope, Scope&) {
                                   node.scope = node_of.find(scope)->second;
                                   return false;
                               });
    }

    // The offered nodes in the order of a walk down the scope tree, so a
    // segment's symbols share their scopes and few of those are copied in.
    std::vector<bool> placed(nodes.size());
    for(std::uint32_t i = 0; i < nodes.size(); i += 1) {
        if(nodes[i].offered) {
            for(auto at = i; at != no_entry && !placed[at]; at = nodes[at].scope) {
                placed[at] = true;
            }
        }
    }
    std::vector<std::uint32_t> child_ends(nodes.size() + 1);
    for(std::uint32_t i = 0; i < nodes.size(); i += 1) {
        if(placed[i] && nodes[i].scope != no_entry) {
            child_ends[nodes[i].scope + 1] += 1;
        }
    }
    std::partial_sum(child_ends.begin(), child_ends.end(), child_ends.begin());
    std::vector<std::uint32_t> children(child_ends.back());
    std::vector<std::uint32_t> roots;
    {
        auto next = child_ends;
        for(std::uint32_t i = 0; i < nodes.size(); i += 1) {
            if(!placed[i]) {
                continue;
            }
            if(nodes[i].scope == no_entry) {
                roots.push_back(i);
            } else {
                children[next[nodes[i].scope]] = i;
                next[nodes[i].scope] += 1;
            }
        }
    }
    auto by_hash = [&](std::uint32_t lhs, std::uint32_t rhs) {
        return nodes[lhs].symbol->hash < nodes[rhs].symbol->hash;
    };
    for(std::uint32_t i = 0; i < nodes.size(); i += 1) {
        ranges::sort(children.begin() + child_ends[i],
                     children.begin() + child_ends[i + 1],
                     by_hash);
    }
    ranges::sort(roots, by_hash);
    std::vector<std::uint32_t> order;
    std::vector<bool> walked(nodes.size());
    std::vector<std::uint32_t> pending;
    auto walk = [&](std::uint32_t root) {
        pending.push_back(root);
        while(!pending.empty()) {
            auto at = pending.back();
            pending.pop_back();
            if(walked[at]) {
                continue;
            }
            walked[at] = true;
            if(nodes[at].offered) {
                order.push_back(at);
            }
            for(auto i = child_ends[at + 1]; i > child_ends[at]; i -= 1) {
                pending.push_back(children[i - 1]);
            }
        }
    };
    for(auto root: roots) {
        walk(root);
    }
    // Scopes linked in a cycle, which only corrupt parent columns form,
    // lead to no root.
    for(std::uint32_t i = 0; i < nodes.size(); i += 1) {
        if(placed[i] && !walked[i]) {
            walk(i);
        }
    }

    std::vector<std::vector<std::byte>> segments;
    llvm::ArrayRef<std::uint32_t> rest = order;
    do {
        auto slice = rest.take_front(bound);
        rest = rest.drop_front(slice.size());
        segments.push_back(encode_segment(nodes,
                                          slice,
                                          segments.empty() ? llvm::ArrayRef(snapshot.covered)
                                                           : llvm::ArrayRef<std::uint32_t>()));
    } while(!rest.empty());
    return segments;
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

/// A mapped segment, its columns checked and its posting lists decoded on
/// first use: a segment no query reaches is never read past its header.
struct SearchIndex::Segment {
    std::unique_ptr<llvm::MemoryBuffer> buffer;

    const Root* columns = nullptr;

    /// The run the segment belongs to.
    std::uint32_t run = 0;

    mutable llvm::DenseMap<std::uint32_t, Bitmap> token_cache;
    mutable llvm::DenseMap<std::uint32_t, Bitmap> children_cache;
    mutable llvm::DenseMap<std::uint32_t, Bitmap> subtree_cache;
    mutable llvm::DenseMap<std::uint32_t, Bitmap> kind_cache;
    mutable llvm::DenseMap<std::uint32_t, Bitmap> file_cache;
    mutable std::optional<Bitmap> top_level_cache;
    mutable std::optional<Bitmap> long_names_cache;

    /// Whether the columns line up, once checked.
    mutable std::optional<bool> usable;

    /// Whether the columns failed their checks or a posting list failed to
    /// decode: the answers since are incomplete, and the owner rebuilds the
    /// index.
    mutable bool damaged = false;

    /// The segment the bytes hold; null when they do not hold one of this
    /// build's formats.
    static std::unique_ptr<Segment> open(std::unique_ptr<llvm::MemoryBuffer> bytes);

    /// Whether the columns line up, which the readers below rely on;
    /// checked on first use. A segment whose columns do not reads as empty.
    bool ready() const {
        if(!usable) {
            auto checked = check();
            if(!checked) {
                LOG_WARN("A search segment fails its checks ({}); the index is rebuilt",
                         std::string_view(checked.error()));
                damaged = true;
            }
            usable = checked.has_value();
        }
        return *usable;
    }

    std::expected<void, llvm::StringRef> check() const;

    /// The entries the segment offers.
    std::uint32_t count() const {
        return static_cast<std::uint32_t>(columns->qualities.size());
    }

    /// The entries, the scopes above the offered ones included.
    std::uint32_t total() const {
        return static_cast<std::uint32_t>(columns->hashes.size());
    }

    llvm::StringRef name(std::uint32_t entry) const {
        return slice(columns->names, columns->name_ends, entry);
    }

    llvm::StringRef arguments(std::uint32_t entry) const {
        return slice(columns->args, columns->args_ends, entry);
    }

    /// The path id of the file an entry of internal linkage lives in;
    /// no_file for any other entry.
    std::uint32_t local_file(std::uint32_t entry) const {
        auto position = sorted_find(columns->local_entries, entry);
        return position ? columns->local_paths[*position] : no_file;
    }

    /// A posting list over `universe` decoded; a malformed one reads as
    /// empty and marks the index damaged.
    Bitmap decode(llvm::ArrayRef<std::uint8_t> arena,
                  llvm::ArrayRef<std::uint32_t> ends,
                  std::uint32_t i,
                  std::uint32_t universe) const {
        auto decoded = read_postings(slice(arena, ends, i), universe);
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
                         std::uint32_t i,
                         std::uint32_t universe) const {
        auto [it, inserted] = cache.try_emplace(i);
        if(inserted) {
            it->second = decode(arena, ends, i, universe);
        }
        return it->second;
    }

    /// The entries carrying a token, null when none does.
    const Bitmap* token(NameToken key) const {
        auto i = sorted_find(columns->token_keys, key);
        if(!i) {
            return nullptr;
        }
        return &cached(token_cache, columns->token_postings, columns->token_ends, *i, count());
    }

    /// The direct members of a container, the top level for no_entry.
    const Bitmap& children(std::uint32_t container) const {
        if(container == no_entry) {
            if(!top_level_cache) {
                std::uint32_t ends[] = {static_cast<std::uint32_t>(columns->top_level.size())};
                top_level_cache = decode(columns->top_level, ends, 0, total());
            }
            return *top_level_cache;
        }
        auto i = sorted_find(columns->container_entries, container);
        if(!i) {
            const static Bitmap none;
            return none;
        }
        return cached(children_cache,
                      columns->children_postings,
                      columns->children_ends,
                      *i,
                      total());
    }

    /// The entries whose name outruns the token bound.
    const Bitmap& long_names() const {
        if(!long_names_cache) {
            std::uint32_t ends[] = {static_cast<std::uint32_t>(columns->long_names.size())};
            long_names_cache = decode(columns->long_names, ends, 0, count());
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
            return cached(subtree_cache,
                          columns->subtree_postings,
                          columns->subtree_ends,
                          *i,
                          total());
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
        return cached(kind_cache, columns->kind_postings, columns->kind_ends, value, count());
    }

    const Bitmap& file(std::uint32_t index) const {
        return cached(file_cache, columns->file_postings, columns->file_ends, index, count());
    }

    /// The entry offering a symbol, none when the segment offers none.
    std::optional<std::uint32_t> offered(SymbolHash hash) const {
        auto& by_hash = columns->by_hash;
        auto it = ranges::lower_bound(by_hash, hash, {}, [&](std::uint32_t entry) {
            return columns->hashes[entry];
        });
        if(it == by_hash.end() || columns->hashes[*it] != hash) {
            return std::nullopt;
        }
        return *it;
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

std::unique_ptr<SearchIndex::Segment>
    SearchIndex::Segment::open(std::unique_ptr<llvm::MemoryBuffer> bytes) {
    bytes = in_place_buffer(std::move(bytes));
    auto root = verify_root<SearchIndexBlob>(bytes->getBuffer(), "search segment");
    if(!root) {
        return nullptr;
    }
    if(root->search_version != search_format_version) {
        LOG_DEBUG("Rejecting search segment: written by another search format version");
        return nullptr;
    }
    auto segment = std::make_unique<Segment>();
    segment->columns = root;
    segment->buffer = std::move(bytes);
    return segment;
}

std::expected<void, llvm::StringRef> SearchIndex::Segment::check() const {
    llvm::ArrayRef<std::uint64_t> hashes = columns->hashes;
    llvm::ArrayRef<std::uint32_t> by_hash = columns->by_hash;

    auto total = hashes.size();
    auto count = columns->qualities.size();
    if(count > total || columns->name_ends.size() != total || columns->args_ends.size() != total ||
       columns->parents.size() != total || columns->by_lower_name.size() != total ||
       by_hash.size() != count) {
        return std::unexpected("entry columns do not line up");
    }
    if(!monotone_ends(columns->name_ends, columns->names.size()) ||
       !monotone_ends(columns->args_ends, columns->args.size())) {
        return std::unexpected("name arenas do not line up");
    }
    for(auto parent: columns->parents) {
        if(parent != no_entry && parent >= total) {
            return std::unexpected("parent outside the entries");
        }
    }
    // FIXME: by_lower_name is range-checked but not checked for its order: a
    // blob out of lowercase-name order loads, and lower_range's binary search
    // misses entries silently.
    for(auto entry: columns->by_lower_name) {
        if(entry >= total) {
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
    if(!sparse_ok(columns->container_entries, columns->children_ends.size(), total) ||
       !monotone_ends(columns->children_ends, columns->children_postings.size())) {
        return std::unexpected("container columns do not line up");
    }
    if(!sparse_ok(columns->subtree_entries, columns->subtree_ends.size(), total) ||
       !monotone_ends(columns->subtree_ends, columns->subtree_postings.size())) {
        return std::unexpected("subtree columns do not line up");
    }
    if(!monotone_ends(columns->kind_ends, columns->kind_postings.size())) {
        return std::unexpected("kind columns do not line up");
    }
    if(columns->paths.size() != columns->file_ends.size() || !strictly_ascending(columns->paths) ||
       !monotone_ends(columns->file_ends, columns->file_postings.size())) {
        return std::unexpected("file columns do not line up");
    }
    if(!sparse_ok(columns->local_entries, columns->local_paths.size(), count) ||
       ranges::contains(columns->local_paths, no_file)) {
        return std::unexpected("local columns do not line up");
    }
    if(!strictly_ascending(columns->covered)) {
        return std::unexpected("covered files are not ascending");
    }
    return {};
}

SearchIndex::SearchIndex() = default;
SearchIndex::~SearchIndex() = default;
SearchIndex::SearchIndex(SearchIndex&&) noexcept = default;
SearchIndex& SearchIndex::operator=(SearchIndex&&) noexcept = default;

bool SearchIndex::load(std::unique_ptr<llvm::MemoryBuffer> segment) {
    clear();
    std::vector<std::unique_ptr<llvm::MemoryBuffer>> run;
    run.push_back(std::move(segment));
    return replace(0, std::move(run));
}

bool SearchIndex::replace(std::uint32_t runs,
                          std::vector<std::unique_ptr<llvm::MemoryBuffer>> bytes) {
    assert(runs <= run_ends.size() && !bytes.empty() && "a run replaces runs the index holds");
    std::vector<std::unique_ptr<Segment>> opened;
    for(auto& segment: bytes) {
        auto bound = Segment::open(std::move(segment));
        if(!bound) {
            return false;
        }
        opened.push_back(std::move(bound));
    }
    auto kept = static_cast<std::uint32_t>(run_ends.size()) - runs;
    segments.resize(kept == 0 ? 0 : run_ends[kept - 1]);
    run_ends.resize(kept);
    for(auto& segment: opened) {
        segment->run = kept;
        segments.push_back(std::move(segment));
    }
    run_ends.push_back(static_cast<std::uint32_t>(segments.size()));
    index_runs();
    return true;
}

bool SearchIndex::rebind(std::uint32_t segment, std::unique_ptr<llvm::MemoryBuffer> bytes) {
    auto& held = *segments[segment];
    if(!rebind_buffer(held.buffer, std::move(bytes))) {
        return false;
    }
    held.columns = &codec::blob::view_verified<SearchIndexBlob>(as_bytes(held.buffer->getBuffer()));
    return true;
}

void SearchIndex::clear() {
    segments.clear();
    run_ends.clear();
    newest_symbols.clear();
    newest_files.clear();
}

void SearchIndex::index_runs() {
    newest_symbols.clear();
    newest_files.clear();
    for(auto& segment: views::drop(segments, run_ends.front())) {
        if(!segment->ready()) {
            continue;
        }
        for(std::uint32_t entry = 0; entry < segment->count(); entry += 1) {
            newest_symbols[segment->columns->hashes[entry]] = segment->run;
        }
        for(auto file: segment->columns->covered) {
            newest_files[file] = segment->run;
        }
    }
}

bool SearchIndex::hidden(const Segment& segment, std::uint32_t entry) const {
    if(segment.run + 1 == run_ends.size()) {
        return false;
    }
    if(auto it = newest_symbols.find(segment.columns->hashes[entry]);
       it != newest_symbols.end() && it->second > segment.run) {
        return true;
    }
    if(auto file = segment.local_file(entry); file != no_file) {
        if(auto it = newest_files.find(file);
           it != newest_files.end() && it->second > segment.run) {
            return true;
        }
    }
    return false;
}

std::pair<const SearchIndex::Segment*, std::uint32_t>
    SearchIndex::newest(const Segment& segment, std::uint32_t entry) const {
    auto hash = segment.columns->hashes[entry];
    auto it = newest_symbols.find(hash);
    if(it == newest_symbols.end() || it->second <= segment.run) {
        return {&segment, entry};
    }
    auto run = it->second;
    for(auto i = run_ends[run - 1]; i < run_ends[run]; i += 1) {
        if(!segments[i]->ready()) {
            continue;
        }
        if(auto found = segments[i]->offered(hash)) {
            return {segments[i].get(), *found};
        }
    }
    std::unreachable();
}

llvm::SmallVector<ScopeEntry, 8> SearchIndex::scope_chain(const Segment& segment,
                                                          std::uint32_t entry) const {
    llvm::SmallVector<ScopeEntry, 8> entries;
    auto [at, current] = newest(segment, entry);
    auto parent = at->columns->parents[current];
    for(std::uint32_t depth = 0; parent != no_entry && depth < max_chain; depth += 1) {
        std::tie(at, current) = newest(*at, parent);
        entries.push_back({.name = at->name(current), .args = at->arguments(current)});
        parent = at->columns->parents[current];
    }
    ranges::reverse(entries);
    return entries;
}

bool SearchIndex::loaded() const {
    return !segments.empty();
}

std::uint32_t SearchIndex::run_count() const {
    return static_cast<std::uint32_t>(run_ends.size());
}

std::uint32_t SearchIndex::run_size(std::uint32_t run) const {
    std::uint32_t size = 0;
    for(auto i = run == 0 ? 0 : run_ends[run - 1]; i < run_ends[run]; i += 1) {
        size +=
            segments[i]->count() + static_cast<std::uint32_t>(segments[i]->columns->covered.size());
    }
    return size;
}

std::uint32_t SearchIndex::size() const {
    std::uint32_t size = 0;
    for(auto& segment: segments) {
        size += segment->count();
    }
    return size;
}

void SearchIndex::for_each_source(std::uint32_t run,
                                  llvm::function_ref<void(SymbolHash)> symbol,
                                  llvm::function_ref<void(std::uint32_t)> file) const {
    for(auto i = run == 0 ? 0 : run_ends[run - 1]; i < run_ends[run]; i += 1) {
        if(!segments[i]->ready()) {
            continue;
        }
        auto& columns = *segments[i]->columns;
        llvm::ArrayRef<std::uint32_t> locals = columns.local_entries;
        for(std::uint32_t entry = 0; entry < segments[i]->count(); entry += 1) {
            if(!locals.empty() && locals.front() == entry) {
                locals = locals.drop_front();
                continue;
            }
            symbol(columns.hashes[entry]);
        }
        for(auto covered: columns.covered) {
            file(covered);
        }
    }
}

bool SearchIndex::damaged() const {
    return ranges::any_of(segments, [](auto& segment) { return segment->damaged; });
}

SearchOutcome SearchIndex::search(const SymbolQuery& query,
                                  std::uint32_t limit,
                                  llvm::function_ref<bool(std::uint32_t)> wanted_file) const {
    if(segments.empty() || limit == 0 || !query.by_pattern()) {
        return {};
    }

    // Whether each file is among the query's paths, judged once a file.
    llvm::DenseMap<std::uint32_t, bool> wanted;
    auto is_wanted = [&](std::uint32_t file) {
        auto [it, inserted] = wanted.try_emplace(file);
        if(inserted) {
            it->second = wanted_file(file);
        }
        return it->second;
    };

    // Per segment, everything a hit must satisfy besides its name — the
    // kinds, the files and the scope — as one bitmap, none when nothing
    // narrows; and the entries already ranked: one the clean pass rejected
    // is judged again by the typo pass, which may accept it.
    std::vector<std::optional<Bitmap>> filters(segments.size());
    std::vector<Bitmap> ranked(segments.size());
    for(std::uint32_t at = 0; at < segments.size(); at += 1) {
        auto& segment = *segments[at];
        auto& filter = filters[at];
        if(!segment.ready()) {
            filter.emplace();
            continue;
        }
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
                allowed |= segment.kind(kind);
            }
            narrow(std::move(allowed));
        }
        if(!query.paths.empty()) {
            Bitmap allowed;
            for(std::uint32_t i = 0; i < segment.columns->paths.size(); i += 1) {
                if(is_wanted(segment.columns->paths[i])) {
                    allowed |= segment.file(i);
                }
            }
            narrow(std::move(allowed));
        }
        if(query.external_only && !segment.columns->local_entries.empty()) {
            Bitmap allowed;
            allowed.addRange(0, segment.count());
            for(auto entry: segment.columns->local_entries) {
                allowed.remove(entry);
            }
            narrow(std::move(allowed));
        }
        if(query.absolute || !query.scope.empty()) {
            // The containers the scope leads to: the top level for an empty
            // absolute scope, else every container named like the scope's
            // last name whose own chain the names before it describe.
            llvm::SmallVector<std::uint32_t, 4> containers;
            if(query.scope.empty()) {
                containers.push_back(no_entry);
            } else {
                auto& last = query.scope.back();
                llvm::ArrayRef<SymbolQuery::Segment> above = query.scope;
                above = above.drop_back();
                for(auto entry: segment.lower_range(llvm::StringRef(last.name).lower())) {
                    auto [spelled, current] = newest(segment, entry);
                    if(segment.is_container(entry) &&
                       args_match(last.args, spelled->arguments(current)) &&
                       segments_match(above, scope_chain(segment, entry), query.absolute)) {
                        containers.push_back(entry);
                    }
                }
            }
            Bitmap allowed;
            for(auto container: containers) {
                allowed |=
                    query.direct() ? segment.children(container) : segment.subtree(container);
            }
            narrow(std::move(allowed));
        }
    }
    auto allowed = [&](std::uint32_t at, std::uint32_t entry) {
        auto& filter = filters[at];
        return entry < segments[at]->count() && (!filter || filter->contains(entry)) &&
               !hidden(*segments[at], entry);
    };
    // The segments some entry of which the filters allow.
    llvm::SmallVector<std::uint32_t> open;
    for(std::uint32_t at = 0; at < segments.size(); at += 1) {
        if(!filters[at] || !filters[at]->isEmpty()) {
            open.push_back(at);
        }
    }

    NameRanker ranker(query);
    TopHits top{.limit = limit};
    auto consider = [&](std::uint32_t at, std::uint32_t entry, bool lenient) {
        auto& segment = *segments[at];
        auto name = segment.name(entry);
        auto args = segment.arguments(entry);
        if(auto rank = ranker.rank(name, args, segment.columns->qualities[entry], lenient)) {
            ranked[at].add(entry);
            top.push({.rank = *rank,
                      .name = name,
                      .args = args,
                      .hash = segment.columns->hashes[entry],
                      .file = segment.local_file(entry)});
        }
    };
    // A segment's entries come in quality order, and no name score exceeds
    // 1 past the exact tier, so once an entry's quality cannot enter the
    // held set, no later entry of its segment can either.
    auto scan = [&](std::uint32_t at, const Bitmap& candidates, bool lenient) {
        auto& segment = *segments[at];
        for(auto entry: candidates) {
            if(entry >= segment.count()) {
                break;
            }
            if(!allowed(at, entry) || ranked[at].contains(entry)) {
                continue;
            }
            if(!top.admits({.tier = static_cast<std::uint8_t>(lenient ? 3 : 2),
                            .score = segment.columns->qualities[entry]})) {
                top.overflowed = true;
                break;
            }
            consider(at, entry, lenient);
        }
    };
    auto scan_everything = [&](bool lenient) {
        for(auto at: open) {
            if(auto& filter = filters[at]) {
                scan(at, *filter, lenient);
            } else {
                Bitmap all;
                all.addRange(0, segments[at]->count());
                scan(at, all, lenient);
            }
        }
    };
    auto consider_named = [&](llvm::StringRef name) {
        auto lower = name.lower();
        for(auto at: open) {
            for(auto entry: segments[at]->lower_range(lower)) {
                if(allowed(at, entry)) {
                    consider(at, entry, false);
                }
            }
        }
    };

    switch(query.mode) {
        case Mode::Members:
        case Mode::Subtree: scan_everything(false); break;
        case Mode::Exact: consider_named(query.pattern); break;
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
                scan_everything(false);
                break;
            }
            ranges::sort(tokens);
            tokens.erase(ranges::unique(tokens).begin(), tokens.end());
            for(auto at: open) {
                auto& segment = *segments[at];
                scan(at,
                     segment.intersection(tokens).value_or(Bitmap{}) | segment.long_names(),
                     false);
            }
            break;
        }
        case Mode::Fuzzy: {
            if(query.pattern.empty()) {
                scan_everything(false);
                break;
            }
            consider_named(query.pattern);
            llvm::SmallVector<NameToken, 16> tokens;
            query_tokens(query.pattern, tokens);
            if(tokens.empty()) {
                scan_everything(false);
                break;
            }
            for(auto at: open) {
                auto& segment = *segments[at];
                scan(at,
                     segment.intersection(tokens).value_or(Bitmap{}) | segment.long_names(),
                     false);
            }
            if(top.full() || !ranker.has_typo_matches()) {
                break;
            }
            llvm::SmallVector<TypoAlternative> alternatives;
            typo_tokens(query.pattern, alternatives);
            for(auto at: open) {
                auto& segment = *segments[at];
                Bitmap candidates = segment.long_names();
                for(auto& alternative: alternatives) {
                    if(auto some = segment.intersection(alternative.tokens)) {
                        candidates |= *some;
                    }
                }
                scan(at, candidates, true);
            }
            break;
        }
    }

    SearchOutcome outcome{.exhausted = !top.overflowed};
    for(auto& hit: std::move(top).sorted()) {
        outcome.hits.push_back({.hash = hit.hash, .rank = hit.rank, .file = hit.file});
    }
    return outcome;
}

}  // namespace clice::index

#include "index/search_index.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <numeric>
#include <string_view>
#include <utility>

#include "index/serialization.h"
#include "support/logging.h"

#include "kota/meta/enum.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/raw_ostream.h"

namespace clice::index {

namespace {

/// Layout and scoring version of the search blob, apart from the index
/// format it is derived from: a bump rebuilds the search index alone.
constexpr std::uint32_t search_format_version = 1;

constexpr std::uint32_t no_doc = ~0u;

/// Deeper parent chains than this only come from corrupt bytes.
constexpr std::size_t max_chain = 64;

using Mode = SymbolQuery::Mode;

/// The persisted search index. Rows are in quality order (ties by hash),
/// which is the doc id order every bitmap speaks; the arenas hold the
/// names back to back with one end offset per row; posting arenas hold
/// portable roaring images the same way.
struct SearchBlob {
    std::uint32_t format_version = 0;
    std::uint32_t search_version = 0;
    /// See SearchSnapshot::generation.
    std::uint64_t generation = 0;

    std::vector<std::uint64_t> hashes;
    std::string names;
    std::vector<std::uint32_t> name_ends;
    std::string args;
    std::vector<std::uint32_t> args_ends;
    /// The doc of the nearest enclosing symbol that is not an inline
    /// namespace, or no_doc at the translation unit or when that symbol
    /// is not in the index.
    std::vector<std::uint32_t> parents;
    std::vector<float> qualities;
    /// Docs sorted by lowercase name, then name, then doc.
    std::vector<std::uint32_t> by_lower_name;
    /// Docs sorted by hash.
    std::vector<std::uint32_t> by_hash;

    std::vector<std::uint32_t> token_keys;
    std::vector<std::uint32_t> token_ends;
    std::vector<std::uint8_t> token_postings;

    /// Docs with members, sorted; their members as bitmaps.
    std::vector<std::uint32_t> container_docs;
    std::vector<std::uint32_t> children_ends;
    std::vector<std::uint8_t> children_postings;
    /// The docs with no parent in the index, one image.
    std::vector<std::uint8_t> top_level;

    /// The docs whose name runs past the token bound (name_bound), one
    /// image: their tail carries no token, so every name lookup scans
    /// them.
    std::vector<std::uint8_t> long_names;

    /// Namespace docs, sorted; everything below each as bitmaps.
    std::vector<std::uint32_t> subtree_docs;
    std::vector<std::uint32_t> subtree_ends;
    std::vector<std::uint8_t> subtree_postings;

    /// One bitmap per SymbolKind value.
    std::vector<std::uint32_t> kind_ends;
    std::vector<std::uint8_t> kind_postings;

    /// The files of the rows, one bitmap each.
    std::vector<std::string> paths;
    std::vector<std::uint32_t> file_ends;
    std::vector<std::uint8_t> file_postings;
};

using BlobView = kota::codec::fbs::table_view<SearchBlob>;

void append_image(std::vector<std::uint8_t>& arena,
                  std::vector<std::uint32_t>& ends,
                  const Bitmap& bitmap) {
    auto image = write_bitmap(bitmap);
    auto* bytes = reinterpret_cast<const std::uint8_t*>(image.data());
    arena.insert(arena.end(), bytes, bytes + image.size());
    ends.push_back(static_cast<std::uint32_t>(arena.size()));
}

std::string lowercase(llvm::StringRef text) {
    return text.lower();
}

bool reserved_name(llvm::StringRef name) {
    return name.size() > 1 && name[0] == '_' && (name[1] == '_' || llvm::isUpper(name[1]));
}

bool strictly_ascending_docs(llvm::ArrayRef<std::uint32_t> docs, std::size_t count) {
    for(std::size_t i = 0; i < docs.size(); i += 1) {
        if(docs[i] >= count || (i > 0 && docs[i] <= docs[i - 1])) {
            return false;
        }
    }
    return true;
}

/// The hits held while a search runs: the worst on top of the heap, so
/// a full set drops it for a better arrival.
struct TopHits {
    struct Ranked {
        NameRank rank;
        std::uint32_t doc;
    };

    std::size_t limit;
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
                           name_of(rhs.doc),
                           args_of(rhs.doc),
                           hash_of(rhs.doc),
                           lhs,
                           lhs_name,
                           lhs_args,
                           lhs_hash);
    }

    bool before(const Ranked& lhs, const Ranked& rhs) const {
        return before(lhs.rank, name_of(lhs.doc), args_of(lhs.doc), hash_of(lhs.doc), rhs);
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
            std::ranges::push_heap(hits, compare);
            return;
        }
        overflowed = true;
        if(!before(hit, hits.front())) {
            return;
        }
        std::ranges::pop_heap(hits, compare);
        hits.back() = hit;
        std::ranges::push_heap(hits, compare);
    }

    std::vector<Ranked> sorted() && {
        std::ranges::sort(hits,
                          [&](const Ranked& lhs, const Ranked& rhs) { return before(lhs, rhs); });
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

std::string build_search_blob(const SearchSnapshot& snapshot) {
    struct Row {
        std::uint32_t entry;
        float quality;
    };

    std::vector<Row> rows;
    llvm::DenseSet<SymbolHash> seen;
    for(std::uint32_t i = 0; i < snapshot.entries.size(); i += 1) {
        auto& entry = snapshot.entries[i];
        if(!is_searchable_kind(entry.kind) || entry.name.empty() || entry.hash == 0 ||
           reserved_key(entry.hash) || !seen.insert(entry.hash).second) {
            continue;
        }
        rows.push_back(
            {i, symbol_quality(entry.name, entry.kind, entry.flags, entry.reference_files)});
    }
    std::ranges::sort(rows, [&](const Row& lhs, const Row& rhs) {
        if(lhs.quality != rhs.quality) {
            return lhs.quality > rhs.quality;
        }
        return snapshot.entries[lhs.entry].hash < snapshot.entries[rhs.entry].hash;
    });
    auto count = static_cast<std::uint32_t>(rows.size());
    auto entry_of = [&](std::uint32_t doc) -> const SearchEntry& {
        return snapshot.entries[rows[doc].entry];
    };

    llvm::DenseMap<SymbolHash, std::uint32_t> doc_of;
    doc_of.reserve(count);
    for(std::uint32_t doc = 0; doc < count; doc += 1) {
        doc_of.try_emplace(entry_of(doc).hash, doc);
    }
    // The chain a qualified name spells skips inline namespaces.
    auto parent_of = [&](std::uint32_t doc) {
        auto parent = entry_of(doc).parent;
        for(std::size_t depth = 0; parent != 0 && depth < max_chain; depth += 1) {
            auto it = doc_of.find(parent);
            if(it == doc_of.end()) {
                return no_doc;
            }
            auto& candidate = entry_of(it->second);
            if(!has_flag(candidate.flags, SymbolFlags::InlineNamespace)) {
                return it->second;
            }
            parent = candidate.parent;
        }
        return no_doc;
    };

    SearchBlob blob;
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
    for(std::uint32_t doc = 0; doc < count; doc += 1) {
        auto& entry = entry_of(doc);
        blob.hashes.push_back(entry.hash);
        blob.names += entry.name;
        blob.name_ends.push_back(static_cast<std::uint32_t>(blob.names.size()));
        blob.args += entry.args;
        blob.args_ends.push_back(static_cast<std::uint32_t>(blob.args.size()));
        blob.parents.push_back(parent_of(doc));
        blob.qualities.push_back(rows[doc].quality);
        lower_names.push_back(lowercase(entry.name));
    }

    blob.by_lower_name.resize(count);
    std::iota(blob.by_lower_name.begin(), blob.by_lower_name.end(), 0u);
    std::ranges::sort(blob.by_lower_name, [&](std::uint32_t lhs, std::uint32_t rhs) {
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
    std::ranges::sort(blob.by_hash, [&](std::uint32_t lhs, std::uint32_t rhs) {
        return entry_of(lhs).hash < entry_of(rhs).hash;
    });

    llvm::DenseMap<NameToken, Bitmap> postings;
    llvm::SmallVector<NameToken, 64> tokens;
    for(std::uint32_t doc = 0; doc < count; doc += 1) {
        name_tokens(entry_of(doc).name, tokens);
        for(auto token: tokens) {
            postings[token].add(doc);
        }
    }
    blob.token_keys.reserve(postings.size());
    for(auto token: llvm::make_first_range(postings)) {
        blob.token_keys.push_back(token);
    }
    llvm::sort(blob.token_keys);
    for(auto token: blob.token_keys) {
        append_image(blob.token_postings, blob.token_ends, postings.find(token)->second);
    }

    llvm::DenseMap<std::uint32_t, Bitmap> children;
    Bitmap top_level;
    for(std::uint32_t doc = 0; doc < count; doc += 1) {
        auto parent = blob.parents[doc];
        if(parent == no_doc) {
            top_level.add(doc);
        } else {
            children[parent].add(doc);
        }
    }
    for(auto container: llvm::make_first_range(children)) {
        blob.container_docs.push_back(container);
    }
    llvm::sort(blob.container_docs);
    for(auto container: blob.container_docs) {
        append_image(blob.children_postings, blob.children_ends, children.find(container)->second);
    }
    auto top_image = write_bitmap(top_level);
    blob.top_level.assign(reinterpret_cast<const std::uint8_t*>(top_image.data()),
                          reinterpret_cast<const std::uint8_t*>(top_image.data()) +
                              top_image.size());
    Bitmap long_names;
    for(std::uint32_t doc = 0; doc < count; doc += 1) {
        if(entry_of(doc).name.size() > name_bound) {
            long_names.add(doc);
        }
    }
    auto long_image = write_bitmap(long_names);
    blob.long_names.assign(reinterpret_cast<const std::uint8_t*>(long_image.data()),
                           reinterpret_cast<const std::uint8_t*>(long_image.data()) +
                               long_image.size());

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
    for(auto container: blob.container_docs) {
        if(entry_of(container).kind == SymbolKind::Namespace) {
            blob.subtree_docs.push_back(container);
            append_image(blob.subtree_postings, blob.subtree_ends, subtree_of(container));
        }
    }

    constexpr auto kind_count = kota::meta::reflection<SymbolKind::Kind>::member_names.size();
    std::vector<Bitmap> kinds(kind_count);
    std::vector<Bitmap> files(snapshot.paths.size());
    for(std::uint32_t doc = 0; doc < count; doc += 1) {
        auto& entry = entry_of(doc);
        if(entry.kind.value() < kind_count) {
            kinds[entry.kind.value()].add(doc);
        }
        if(entry.file != no_file && entry.file < files.size()) {
            files[entry.file].add(doc);
        }
    }
    for(auto& bitmap: kinds) {
        append_image(blob.kind_postings, blob.kind_ends, bitmap);
    }
    blob.paths = snapshot.paths;
    for(auto& bitmap: files) {
        append_image(blob.file_postings, blob.file_ends, bitmap);
    }

    std::string bytes;
    llvm::raw_string_ostream os(bytes);
    serialize_blob(blob, os);
    return bytes;
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
    query(query), lower_pattern(lowercase(query.pattern)),
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
    if(name.size() == lower_pattern.size() && lowercase(name) == lower_pattern) {
        return NameRank{.tier = static_cast<std::uint8_t>(name == query.pattern ? 0 : 1),
                        .score = quality};
    }
    // A query too short for trigrams is keyed by a name's first two words
    // in the index; judging rows the same way keeps the two answers alike.
    if(!short_tokens.empty()) {
        llvm::SmallVector<NameToken, 64> tokens;
        name_tokens(name, tokens);
        if(!llvm::all_of(short_tokens, [&](NameToken token) {
               return std::ranges::binary_search(tokens, token);
           })) {
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

/// The mapped blob with its columns bound and its bitmaps viewed in
/// place on first use.
struct SearchIndex::View {
    std::unique_ptr<llvm::MemoryBuffer> buffer;
    std::uint64_t generation = 0;

    llvm::ArrayRef<std::uint64_t> hashes;
    llvm::StringRef names;
    llvm::ArrayRef<std::uint32_t> name_ends;
    llvm::StringRef args;
    llvm::ArrayRef<std::uint32_t> args_ends;
    llvm::ArrayRef<std::uint32_t> parents;
    llvm::ArrayRef<float> qualities;
    llvm::ArrayRef<std::uint32_t> by_lower_name;
    llvm::ArrayRef<std::uint32_t> by_hash;
    llvm::ArrayRef<std::uint32_t> token_keys;
    llvm::ArrayRef<std::uint32_t> token_ends;
    llvm::ArrayRef<std::uint8_t> token_postings;
    llvm::ArrayRef<std::uint32_t> container_docs;
    llvm::ArrayRef<std::uint32_t> children_ends;
    llvm::ArrayRef<std::uint8_t> children_postings;
    llvm::ArrayRef<std::uint8_t> top_level_image;
    llvm::ArrayRef<std::uint8_t> long_names_image;
    llvm::ArrayRef<std::uint32_t> subtree_docs;
    llvm::ArrayRef<std::uint32_t> subtree_ends;
    llvm::ArrayRef<std::uint8_t> subtree_postings;
    llvm::ArrayRef<std::uint32_t> kind_ends;
    llvm::ArrayRef<std::uint8_t> kind_postings;
    kota::codec::fbs::array_view<std::string> paths;
    llvm::ArrayRef<std::uint32_t> file_ends;
    llvm::ArrayRef<std::uint8_t> file_postings;

    mutable llvm::DenseMap<std::uint32_t, Bitmap> token_cache;
    mutable llvm::DenseMap<std::uint32_t, Bitmap> children_cache;
    mutable llvm::DenseMap<std::uint32_t, Bitmap> subtree_cache;
    mutable llvm::DenseMap<std::uint32_t, Bitmap> kind_cache;
    mutable llvm::DenseMap<std::uint32_t, Bitmap> file_cache;
    mutable std::optional<Bitmap> top_level_cache;
    mutable std::optional<Bitmap> long_names_cache;

    /// Whether a posting image failed to decode: the answers since are
    /// incomplete, and the owner rebuilds the index.
    mutable bool damaged = false;

    /// Bind the columns of a verified blob, checking they line up.
    std::expected<void, llvm::StringRef> bind(BlobView root);

    std::uint32_t count() const {
        return static_cast<std::uint32_t>(hashes.size());
    }

    static llvm::StringRef slice(llvm::StringRef arena,
                                 llvm::ArrayRef<std::uint32_t> ends,
                                 std::uint32_t i) {
        auto begin = i == 0 ? 0 : ends[i - 1];
        return arena.slice(begin, ends[i]);
    }

    llvm::StringRef name(std::uint32_t doc) const {
        return slice(names, name_ends, doc);
    }

    llvm::StringRef arguments(std::uint32_t doc) const {
        return slice(args, args_ends, doc);
    }

    /// A posting image viewed in place; a malformed one reads as empty
    /// and marks the index damaged.
    Bitmap decode(llvm::ArrayRef<std::uint8_t> arena,
                  llvm::ArrayRef<std::uint32_t> ends,
                  std::uint32_t i) const {
        auto begin = i == 0 ? 0 : ends[i - 1];
        auto decoded = view_bitmap(arena.data() + begin, ends[i] - begin);
        if(!decoded || (!decoded->isEmpty() && decoded->maximum() >= count())) {
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

    /// The docs carrying a token, null when none does.
    const Bitmap* token(NameToken key) const {
        auto it = std::ranges::lower_bound(token_keys, key);
        if(it == token_keys.end() || *it != key) {
            return nullptr;
        }
        auto i = static_cast<std::uint32_t>(it - token_keys.begin());
        return &cached(token_cache, token_postings, token_ends, i);
    }

    /// The direct members of a container, the top level for no_doc.
    const Bitmap& children(std::uint32_t container) const {
        if(container == no_doc) {
            if(!top_level_cache) {
                std::uint32_t ends[] = {static_cast<std::uint32_t>(top_level_image.size())};
                top_level_cache = decode(top_level_image, ends, 0);
            }
            return *top_level_cache;
        }
        auto it = std::ranges::lower_bound(container_docs, container);
        if(it == container_docs.end() || *it != container) {
            const static Bitmap none;
            return none;
        }
        auto i = static_cast<std::uint32_t>(it - container_docs.begin());
        return cached(children_cache, children_postings, children_ends, i);
    }

    /// The docs whose name outruns the token bound.
    const Bitmap& long_names() const {
        if(!long_names_cache) {
            std::uint32_t ends[] = {static_cast<std::uint32_t>(long_names_image.size())};
            long_names_cache = decode(long_names_image, ends, 0);
        }
        return *long_names_cache;
    }

    bool is_container(std::uint32_t doc) const {
        return std::ranges::binary_search(container_docs, doc);
    }

    /// Everything below a container: stored for namespaces, everything
    /// for the top level, gathered from the members of other containers.
    Bitmap subtree(std::uint32_t container) const {
        if(container == no_doc) {
            Bitmap all;
            all.addRange(0, count());
            return all;
        }
        auto it = std::ranges::lower_bound(subtree_docs, container);
        if(it != subtree_docs.end() && *it == container) {
            auto i = static_cast<std::uint32_t>(it - subtree_docs.begin());
            return cached(subtree_cache, subtree_postings, subtree_ends, i);
        }
        Bitmap below = children(container);
        llvm::SmallVector<std::uint32_t> pending(below.begin(), below.end());
        llvm::DenseSet<std::uint32_t> visited{container};
        while(!pending.empty()) {
            auto doc = pending.pop_back_val();
            if(!visited.insert(doc).second || !is_container(doc)) {
                continue;
            }
            for(auto member: children(doc)) {
                below.add(member);
                pending.push_back(member);
            }
        }
        return below;
    }

    const Bitmap& kind(SymbolKind kind) const {
        auto value = static_cast<std::uint32_t>(kind.value());
        if(value >= kind_ends.size()) {
            const static Bitmap none;
            return none;
        }
        return cached(kind_cache, kind_postings, kind_ends, value);
    }

    const Bitmap& file(std::uint32_t index) const {
        return cached(file_cache, file_postings, file_ends, index);
    }

    /// The chain of containers above a doc, outermost first.
    llvm::SmallVector<ScopeEntry, 8> chain(std::uint32_t doc) const {
        llvm::SmallVector<ScopeEntry, 8> entries;
        auto parent = parents[doc];
        for(std::size_t depth = 0; parent != no_doc && depth < max_chain; depth += 1) {
            entries.push_back({.name = name(parent), .args = arguments(parent)});
            parent = parents[parent];
        }
        std::ranges::reverse(entries);
        return entries;
    }

    /// The docs whose lowercase name is `lower`, in by_lower_name order.
    llvm::ArrayRef<std::uint32_t> lower_range(llvm::StringRef lower) const {
        auto project = [&](std::uint32_t doc) {
            return lowercase(name(doc));
        };
        auto range = std::ranges::equal_range(by_lower_name, lower.str(), {}, project);
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
        llvm::sort(lists, [](const Bitmap* lhs, const Bitmap* rhs) {
            return lhs->cardinality() < rhs->cardinality();
        });
        Bitmap result = *lists.front();
        for(auto* list: llvm::drop_begin(lists)) {
            result &= *list;
            if(result.isEmpty()) {
                break;
            }
        }
        return result;
    }
};

std::expected<void, llvm::StringRef> SearchIndex::View::bind(BlobView root) {
    auto& view = *this;
    if(root[&SearchBlob::format_version] != index_format_version ||
       root[&SearchBlob::search_version] != search_format_version) {
        return std::unexpected("written by another format version");
    }
    view.generation = root[&SearchBlob::generation];
    view.hashes = to_array_ref(root[&SearchBlob::hashes]);
    view.names = to_ref(root[&SearchBlob::names]);
    view.name_ends = to_array_ref(root[&SearchBlob::name_ends]);
    view.args = to_ref(root[&SearchBlob::args]);
    view.args_ends = to_array_ref(root[&SearchBlob::args_ends]);
    view.parents = to_array_ref(root[&SearchBlob::parents]);
    view.qualities = to_array_ref(root[&SearchBlob::qualities]);
    view.by_lower_name = to_array_ref(root[&SearchBlob::by_lower_name]);
    view.by_hash = to_array_ref(root[&SearchBlob::by_hash]);
    view.token_keys = to_array_ref(root[&SearchBlob::token_keys]);
    view.token_ends = to_array_ref(root[&SearchBlob::token_ends]);
    view.token_postings = to_array_ref(root[&SearchBlob::token_postings]);
    view.container_docs = to_array_ref(root[&SearchBlob::container_docs]);
    view.children_ends = to_array_ref(root[&SearchBlob::children_ends]);
    view.children_postings = to_array_ref(root[&SearchBlob::children_postings]);
    view.top_level_image = to_array_ref(root[&SearchBlob::top_level]);
    view.long_names_image = to_array_ref(root[&SearchBlob::long_names]);
    view.subtree_docs = to_array_ref(root[&SearchBlob::subtree_docs]);
    view.subtree_ends = to_array_ref(root[&SearchBlob::subtree_ends]);
    view.subtree_postings = to_array_ref(root[&SearchBlob::subtree_postings]);
    view.kind_ends = to_array_ref(root[&SearchBlob::kind_ends]);
    view.kind_postings = to_array_ref(root[&SearchBlob::kind_postings]);
    view.paths = root[&SearchBlob::paths];
    view.file_ends = to_array_ref(root[&SearchBlob::file_ends]);
    view.file_postings = to_array_ref(root[&SearchBlob::file_postings]);

    auto count = view.hashes.size();
    if(view.name_ends.size() != count || view.args_ends.size() != count ||
       view.parents.size() != count || view.qualities.size() != count ||
       view.by_lower_name.size() != count || view.by_hash.size() != count) {
        return std::unexpected("row columns do not line up");
    }
    if(!monotone_ends(view.name_ends, view.names.size()) ||
       !monotone_ends(view.args_ends, view.args.size())) {
        return std::unexpected("name arenas do not line up");
    }
    for(auto parent: view.parents) {
        if(parent != no_doc && parent >= count) {
            return std::unexpected("parent outside the rows");
        }
    }
    for(auto doc: view.by_lower_name) {
        if(doc >= count) {
            return std::unexpected("name order outside the rows");
        }
    }
    for(std::size_t i = 0; i < count; i += 1) {
        auto doc = view.by_hash[i];
        if(doc >= count || (i > 0 && view.hashes[doc] <= view.hashes[view.by_hash[i - 1]])) {
            return std::unexpected("hash order is not ascending");
        }
    }
    // Hashes become table keys downstream (symbol_info), which reserve
    // two sentinel values.
    for(auto hash: view.hashes) {
        if(reserved_key(hash)) {
            return std::unexpected("reserved symbol hash");
        }
    }
    if(view.token_keys.size() != view.token_ends.size() ||
       !monotone_ends(view.token_ends, view.token_postings.size()) ||
       !std::ranges::is_sorted(view.token_keys, std::less<>{}) ||
       std::ranges::adjacent_find(view.token_keys) != view.token_keys.end()) {
        return std::unexpected("token columns do not line up");
    }
    if(view.container_docs.size() != view.children_ends.size() ||
       !strictly_ascending_docs(view.container_docs, count) ||
       !monotone_ends(view.children_ends, view.children_postings.size())) {
        return std::unexpected("container columns do not line up");
    }
    if(view.subtree_docs.size() != view.subtree_ends.size() ||
       !strictly_ascending_docs(view.subtree_docs, count) ||
       !monotone_ends(view.subtree_ends, view.subtree_postings.size())) {
        return std::unexpected("subtree columns do not line up");
    }
    if(!monotone_ends(view.kind_ends, view.kind_postings.size())) {
        return std::unexpected("kind columns do not line up");
    }
    if(view.paths.size() != view.file_ends.size() ||
       !monotone_ends(view.file_ends, view.file_postings.size())) {
        return std::unexpected("file columns do not line up");
    }
    return {};
}

SearchIndex::SearchIndex() = default;
SearchIndex::~SearchIndex() = default;
SearchIndex::SearchIndex(SearchIndex&&) noexcept = default;
SearchIndex& SearchIndex::operator=(SearchIndex&&) noexcept = default;

bool SearchIndex::load(std::unique_ptr<llvm::MemoryBuffer> blob) {
    clear();
    if(!blob) {
        return false;
    }
    auto root = BlobView::from_bytes(blob_bytes(blob->getBuffer()));
    if(!root.valid()) {
        LOG_DEBUG("Rejecting search blob: structural verification failed");
        return false;
    }
    auto bound = std::make_unique<View>();
    if(auto ok = bound->bind(root); !ok) {
        LOG_DEBUG("Rejecting search blob: {}", std::string_view(ok.error()));
        return false;
    }
    bound->buffer = std::move(blob);
    view = std::move(bound);
    return true;
}

void SearchIndex::clear() {
    view.reset();
}

bool SearchIndex::loaded() const {
    return view != nullptr;
}

std::size_t SearchIndex::size() const {
    return view ? view->count() : 0;
}

std::uint64_t SearchIndex::generation() const {
    return view ? view->generation : 0;
}

bool SearchIndex::damaged() const {
    return view && view->damaged;
}

bool SearchIndex::contains(SymbolHash hash) const {
    if(!view) {
        return false;
    }
    return std::ranges::binary_search(view->by_hash, hash, {}, [&](std::uint32_t doc) {
        return view->hashes[doc];
    });
}

SearchOutcome SearchIndex::search(const SymbolQuery& query, std::size_t limit) const {
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
        for(std::uint32_t i = 0; i < index.paths.size(); i += 1) {
            auto path = index.paths[i];
            if(llvm::any_of(query.paths, [&](const std::string& wanted) {
                   return path_matches(wanted, to_ref(path));
               })) {
                allowed |= index.file(i);
            }
        }
        narrow(std::move(allowed));
    }
    if(query.absolute || !query.scope.empty()) {
        // The containers the scope leads to: the top level for an empty
        // absolute scope, else every container named like the last
        // segment whose own chain the earlier segments describe.
        llvm::SmallVector<std::uint32_t, 4> containers;
        if(query.scope.empty()) {
            containers.push_back(no_doc);
        } else {
            auto& last = query.scope.back();
            llvm::ArrayRef<SymbolQuery::Segment> above = query.scope;
            above = above.drop_back();
            for(auto doc: index.lower_range(lowercase(last.name))) {
                if(index.is_container(doc) && args_match(last.args, index.arguments(doc)) &&
                   segments_match(above, index.chain(doc), query.absolute)) {
                    containers.push_back(doc);
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
    auto allowed = [&](std::uint32_t doc) {
        return !filter || filter->contains(doc);
    };

    NameRanker ranker(query);
    auto name_of = [&](std::uint32_t doc) {
        return index.name(doc);
    };
    auto args_of = [&](std::uint32_t doc) {
        return index.arguments(doc);
    };
    auto hash_of = [&](std::uint32_t doc) {
        return index.hashes[doc];
    };
    TopHits top{.limit = limit, .name_of = name_of, .args_of = args_of, .hash_of = hash_of};
    // The docs already ranked; a doc the clean pass rejected is judged
    // again by the typo pass, which may accept it.
    Bitmap ranked;
    auto consider = [&](std::uint32_t doc, bool lenient) {
        if(auto rank =
               ranker.rank(index.name(doc), index.arguments(doc), index.qualities[doc], lenient)) {
            ranked.add(doc);
            top.push({.rank = *rank, .doc = doc});
        }
    };
    // Docs come in quality order, and no name score exceeds 1 past the
    // exact tier, so once a doc's quality cannot enter the held set, no
    // later doc can either.
    auto scan = [&](const Bitmap& candidates, bool lenient) {
        for(auto doc: candidates) {
            if(doc >= index.count()) {
                break;
            }
            if(!allowed(doc) || ranked.contains(doc)) {
                continue;
            }
            if(!top.admits({.tier = static_cast<std::uint8_t>(lenient ? 3 : 2),
                            .score = index.qualities[doc]})) {
                top.overflowed = true;
                break;
            }
            consider(doc, lenient);
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
            for(auto doc: index.lower_range(lowercase(query.pattern))) {
                if(allowed(doc)) {
                    consider(doc, false);
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
            llvm::sort(tokens);
            tokens.erase(llvm::unique(tokens), tokens.end());
            scan(index.intersection(tokens).value_or(Bitmap{}) | index.long_names(), false);
            break;
        }
        case Mode::Fuzzy: {
            if(query.pattern.empty()) {
                scan(everything(), false);
                break;
            }
            for(auto doc: index.lower_range(lowercase(query.pattern))) {
                if(allowed(doc)) {
                    consider(doc, false);
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
        outcome.hits.push_back({.hash = index.hashes[ranked.doc], .rank = ranked.rank});
    }
    return outcome;
}

}  // namespace clice::index

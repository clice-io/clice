module;

/// The name search index: the project's symbols laid out for lookup by
/// name — quality-ordered entries, a posting list per name token (the
/// matcher's paths through a name, see support/fuzzy_matcher.cppm), and
/// bitmaps per container, kind and file — so a query narrows to
/// candidates by bitmap algebra and scores only those, stopping once the
/// remaining quality cannot reach the results already held.
///
/// The index is runs of segments, oldest first, each run built from a
/// snapshot of some symbols and read as blobs: a short-lived reader maps
/// them and answers at once. A newer run hides what older ones say of the
/// symbols it offers and of the names of internal linkage of the files it
/// covers, so the owner records a change as a small run and folds runs
/// together now and then. Each segment holds a bounded number of entries
/// (segment_entries): a store rewrites a small run, never one big value.
/// Symbols merged after the newest run's snapshot are not in it; the owner
/// keeps their hashes aside and scans those directly.

module clice:index.search_index;

import :index.symbol;
import :index.symbol_query;
import :support.bitmap;
import :support.fuzzy_matcher;
import :support.prelude;

namespace clice::index {

/// Whether symbols of a kind are looked up by name at all.
bool is_searchable_kind(SymbolKind kind);

/// Whether the name search offers a symbol: of a searchable kind, and named
/// by its declaration rather than unnamed.
inline bool searchable(SymbolKind kind, llvm::StringRef name, SymbolFlags flags) {
    return is_searchable_kind(kind) && !name.empty() && !has_flag(flags, SymbolFlags::Unnamed);
}

/// A symbol's standing among results, independent of the query, in
/// (0, 2]: up with the number of files referencing it, down for
/// namespaces, macros, operators, constructors, deprecated or reserved
/// names, macro-spelled, system-header or declaration-only symbols.
float symbol_quality(llvm::StringRef name,
                     SymbolKind kind,
                     SymbolFlags flags,
                     std::uint32_t reference_files);

/// One symbol as the builder takes it.
struct SearchEntry {
    SymbolHash hash = 0;
    std::string name;
    std::string args;
    SymbolHash parent = 0;
    SymbolKind kind;
    SymbolFlags flags = SymbolFlags::None;
    /// The path id of the file declaring it, or no_file.
    std::uint32_t file = no_file;
    std::uint32_t reference_files = 0;
    /// Of internal linkage: no symbol record names the symbol, whose
    /// entries live in `file`'s shard.
    bool local = false;
};

/// What one run of the index is built from.
struct SearchSnapshot {
    /// The symbols the run offers; those of other kinds or without a name
    /// are left out, and a symbol listed twice is offered once, as first
    /// listed.
    std::vector<SearchEntry> entries;
    /// Symbols above the entries that the run does not offer, through
    /// which their qualified names are spelled.
    std::vector<SearchEntry> scopes;
    /// The files whose names of internal linkage `entries` lists in full
    /// (path ids, ascending).
    std::vector<std::uint32_t> covered;
};

/// How many symbols a segment offers at most.
constexpr std::uint32_t segment_entries = 16384;

/// Encode a run's segments, each offering at most `bound` symbols; one
/// segment at least.
std::vector<std::vector<std::byte>> build_search_segments(const SearchSnapshot& snapshot,
                                                          std::uint32_t bound = segment_entries);

/// How a symbol answers a query's name pattern.
struct NameRank {
    /// 0 the exact name; 1 the exact name in another case; 2 a fuzzy,
    /// glob or listed match; 3 a match one typo away.
    std::uint8_t tier = 2;

    /// Within a tier: the name's match score times the symbol's quality,
    /// or 0 for a listing, which orders by name alone.
    float score = 0;
};

/// Whether `lhs` comes after `rhs` among results: tier, then score, then
/// the name and a specialization's arguments, then the hash.
bool ranks_after(const NameRank& lhs,
                 llvm::StringRef lhs_name,
                 llvm::StringRef lhs_args,
                 SymbolHash lhs_hash,
                 const NameRank& rhs,
                 llvm::StringRef rhs_name,
                 llvm::StringRef rhs_args,
                 SymbolHash rhs_hash);

/// Ranks names against one query; built once per query, since the
/// matcher is.
class NameRanker {
public:
    explicit NameRanker(const SymbolQuery& query);

    /// The rank of a symbol named `name` with `args` and standing
    /// `quality` — none when it does not match the pattern. `lenient`
    /// also accepts a name one typo away, at tier 3.
    std::optional<NameRank>
        rank(llvm::StringRef name, llvm::StringRef args, float quality, bool lenient = false);

    /// Whether the query's pattern is long enough for typo matches.
    bool has_typo_matches() const {
        return typo.has_value();
    }

private:
    const SymbolQuery& query;

    std::string lower_pattern;

    FuzzyMatcher exact;

    std::optional<FuzzyMatcher> typo;

    /// The query's tokens when it is too short for trigrams, else empty.
    llvm::SmallVector<NameToken, 2> short_tokens;
};

struct SearchHit {
    SymbolHash hash = 0;
    NameRank rank;
    /// The path id of the file holding a symbol of internal linkage
    /// (SearchEntry::local), which a lookup of the symbol starts from;
    /// no_file for a project symbol.
    std::uint32_t file = no_file;
};

/// A search's answer: its best hits, and whether they are all of them —
/// false when the limit cut the ranking, so a caller wanting more asks
/// again with a wider one.
struct SearchOutcome {
    std::vector<SearchHit> hits;
    bool exhausted = true;
};

class SearchIndex {
public:
    SearchIndex();
    ~SearchIndex();
    SearchIndex(SearchIndex&&) noexcept;
    SearchIndex& operator=(SearchIndex&&) noexcept;

    /// Adopt one segment as the only run (a TU's name table), copied when
    /// it is not readable_in_place. False — and the index stays empty —
    /// when the bytes are not a search segment of this build's formats; a
    /// segment whose columns are inconsistent is found at its first search
    /// and marks the index damaged.
    bool load(std::unique_ptr<llvm::MemoryBuffer> segment);

    /// Replace the newest `runs` runs by one made of `segments`, appending
    /// it when `runs` is 0. False, the index unchanged, when a segment does
    /// not load.
    bool replace(std::uint32_t runs, std::vector<std::unique_ptr<llvm::MemoryBuffer>> segments);

    /// Swap a segment's bytes for byte-identical ones elsewhere (a newer
    /// read snapshot); false, keeping the old ones, when the sizes differ.
    bool rebind(std::uint32_t segment, std::unique_ptr<llvm::MemoryBuffer> bytes);

    void clear();

    bool loaded() const;

    std::uint32_t run_count() const;

    /// How many symbols a run's segments offer, hidden ones included, and
    /// files they cover: what a fold of the run reads again.
    std::uint32_t run_size(std::uint32_t run) const;

    /// Number of symbols the segments offer, hidden ones included.
    std::uint32_t size() const;

    /// Visit what a run is built from: the symbols of external linkage it
    /// offers and the files it covers.
    void for_each_source(std::uint32_t run,
                         llvm::function_ref<void(SymbolHash)> symbol,
                         llvm::function_ref<void(std::uint32_t)> file) const;

    /// Whether a segment failed its checks or a posting list failed to
    /// decode since the load: the answers are incomplete until the owner
    /// rebuilds the index, and a reader scans the table instead.
    bool damaged() const;

    /// At most `limit` hits, best first. Empty for a query by id or
    /// place. `wanted_file` says whether a file (a path id) is one of the
    /// query's paths; it is asked only when the query names paths.
    SearchOutcome search(const SymbolQuery& query,
                         std::uint32_t limit,
                         llvm::function_ref<bool(std::uint32_t)> wanted_file) const;

private:
    struct Segment;

    /// Whether a newer run than the segment's hides its entry.
    bool hidden(const Segment& segment, std::uint32_t entry) const;

    /// The segment and entry of the newest run offering an entry's symbol.
    std::pair<const Segment*, std::uint32_t> newest(const Segment& segment,
                                                    std::uint32_t entry) const;

    /// The chain of containers above an entry, outermost first, as the
    /// newest runs spell them: an older run keeps the spelling its scopes
    /// had when it was built.
    llvm::SmallVector<ScopeEntry, 8> scope_chain(const Segment& segment, std::uint32_t entry) const;

    /// Index the symbols and files the runs after the first offer and
    /// cover by the newest run doing so.
    void index_runs();

private:
    std::vector<std::unique_ptr<Segment>> segments;

    /// Where each run ends among the segments.
    llvm::SmallVector<std::uint32_t> run_ends;

    /// The symbols the runs after the first offer, and the files they
    /// cover, by the newest run that does.
    llvm::DenseMap<SymbolHash, std::uint32_t> newest_symbols;

    llvm::DenseMap<std::uint32_t, std::uint32_t> newest_files;
};

}  // namespace clice::index

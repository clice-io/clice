#pragma once

/// The name search index: the project's symbols laid out for lookup by
/// name — quality-ordered rows, a posting list per name token (the
/// matcher's paths through a name, see support/fuzzy_matcher.h), and
/// bitmaps per container, kind and file — so a query narrows to
/// candidates by bitmap algebra and scores only those, stopping once the
/// remaining quality cannot reach the results already held.
///
/// The index is built from a snapshot of the symbol table and read as a
/// blob: a short-lived reader maps it and answers at once. Symbols merged
/// after the snapshot are not in it; the owner keeps their hashes aside
/// and scans those directly.

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "index/symbol_query.h"
#include "index/types.h"
#include "support/bitmap.h"
#include "support/fuzzy_matcher.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/MemoryBuffer.h"

namespace clice::index {

/// Whether symbols of a kind are looked up by name at all.
bool is_searchable_kind(SymbolKind kind);

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
    /// Index into SearchSnapshot::paths, or no_file.
    std::uint32_t file = no_file;
    std::uint32_t reference_files = 0;
};

/// The symbol table as the builder sees it: rows plus the path table
/// their files index.
struct SearchSnapshot {
    std::vector<SearchEntry> entries;
    std::vector<std::string> paths;
};

/// Encode the search blob for a snapshot; symbols of other kinds or
/// without a name are left out.
std::string build_search_blob(const SearchSnapshot& snapshot);

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
/// the name, then the hash.
bool ranks_after(const NameRank& lhs,
                 llvm::StringRef lhs_name,
                 SymbolHash lhs_hash,
                 const NameRank& rhs,
                 llvm::StringRef rhs_name,
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
};

struct SearchHit {
    SymbolHash hash = 0;
    NameRank rank;
};

class SearchIndex {
public:
    SearchIndex();
    ~SearchIndex();
    SearchIndex(SearchIndex&&) noexcept;
    SearchIndex& operator=(SearchIndex&&) noexcept;

    /// Adopt a blob. False — and the index stays empty — when the bytes
    /// are not a search blob of this build's formats or are inconsistent.
    bool load(std::unique_ptr<llvm::MemoryBuffer> blob);

    void clear();

    bool loaded() const;

    /// Number of symbols in the index.
    std::size_t size() const;

    bool contains(SymbolHash hash) const;

    /// At most `limit` hits, best first. Empty for a query by id or
    /// place.
    std::vector<SearchHit> search(const SymbolQuery& query, std::size_t limit) const;

private:
    struct View;
    std::unique_ptr<View> view;
};

}  // namespace clice::index

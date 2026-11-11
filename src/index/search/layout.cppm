/// The persisted layout of a search segment.

module clice:index.search.layout;

import :support.prelude;

namespace clice::index {

/// Layout and scoring version of the search segments, apart from the index
/// format they are derived from: a bump rebuilds the search index alone.
constexpr std::uint32_t search_format_version = 4;

/// One segment of the persisted search index. Its first entries, as many as
/// it has qualities, are the symbols it offers, in quality order (ties by
/// hash); the rest are the scopes above them it does not offer, so the
/// qualified names of its symbols resolve within it. The arenas hold the
/// names back to back with one end offset per entry; posting arenas hold
/// posting lists over the entries (write_postings) the same way — over the
/// offered entries, or over all of them for the scope structure.
template <template <typename> typename Seq>
struct SearchIndexBlobOf {
    std::uint32_t format_version = 0;
    std::uint32_t search_version = 0;

    Seq<std::uint64_t> hashes;
    Seq<char> names;
    Seq<std::uint32_t> name_ends;
    Seq<char> args;
    Seq<std::uint32_t> args_ends;
    /// The entry of the nearest enclosing symbol a qualified name spells,
    /// or no_entry at the translation unit or when that symbol is not in
    /// the index.
    Seq<std::uint32_t> parents;
    Seq<float> qualities;
    /// All entries sorted by lowercase name, then name, then entry.
    Seq<std::uint32_t> by_lower_name;
    /// The offered entries sorted by hash.
    Seq<std::uint32_t> by_hash;

    Seq<std::uint32_t> token_keys;
    Seq<std::uint32_t> token_ends;
    Seq<std::uint8_t> token_postings;

    /// Entries with members, sorted; their members as bitmaps over all
    /// entries.
    Seq<std::uint32_t> container_entries;
    Seq<std::uint32_t> children_ends;
    Seq<std::uint8_t> children_postings;
    /// The entries with no parent in the segment, over all entries.
    Seq<std::uint8_t> top_level;

    /// The entries whose name runs past the token bound (name_bound), one
    /// posting list: their tail carries no token, so every name lookup scans
    /// them.
    Seq<std::uint8_t> long_names;

    /// Namespace entries, sorted; everything below each as bitmaps over all
    /// entries.
    Seq<std::uint32_t> subtree_entries;
    Seq<std::uint32_t> subtree_ends;
    Seq<std::uint8_t> subtree_postings;

    /// One bitmap per SymbolKind value.
    Seq<std::uint32_t> kind_ends;
    Seq<std::uint8_t> kind_postings;

    /// The files of the entries (path ids), ascending, one bitmap each.
    Seq<std::uint32_t> paths;
    Seq<std::uint32_t> file_ends;
    Seq<std::uint8_t> file_postings;

    /// The entries of internal linkage, sorted, and the path id of the file
    /// each one's entries live in.
    Seq<std::uint32_t> local_entries;
    Seq<std::uint32_t> local_paths;

    /// The files (path ids), ascending, whose names of internal linkage the
    /// segment's run lists in full: the entries of internal linkage older
    /// runs hold in them are gone. A run's first segment carries them.
    Seq<std::uint32_t> covered;
};

using SearchIndexBlob = SearchIndexBlobOf<codec::blob::Owned>;

}  // namespace clice::index

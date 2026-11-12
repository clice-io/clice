/// The persisted layout of a TU index: what indexing one unit sends, and a
/// PCH's index keeps.

module clice:index.indexing.layout;

import :index.symbol;
import :support.prelude;
import :syntax.token;

namespace clice::index {

/// One file's index in the TU index blob: a self-contained single-variant shard
/// blob (index/shard.cppm). `hash` is xxh3 of `blob` — the variant's
/// identity — so the master can skip blobs it already stores without
/// touching their bytes; the blob is left empty for a variant the master
/// said it stores.
template <template <typename> typename Seq>
struct FileSectionOf {
    std::uint32_t path_id = 0;

    std::uint64_t hash = 0;

    Seq<std::byte> blob;
};

using FileSection = FileSectionOf<codec::blob::Owned>;

/// One file's instantiations in the TU index: the instantiation blob of what
/// the run resolved there, empty when it resolved nothing, and the known keys
/// whose definition the file holds that the run skipped, ascending.
template <template <typename> typename Seq>
struct InstantiationSectionOf {
    std::uint32_t path_id = 0;

    Seq<std::byte> blob;

    Seq<std::uint64_t> skipped;
};

using InstantiationSection = InstantiationSectionOf<codec::blob::Owned>;

/// The TU index's serialized layout. Only the indexer materializes it (and
/// tests planting shapes it never emits); every consumer reads the bytes
/// through the TUIndex reader.
template <template <typename> typename Seq>
struct TUIndexBlobOf {
    /// Serialized schema version (index_format_version), gated by
    /// TUIndex::from_bytes.
    /// A worker respawned after the binary on disk changed can be one
    /// build ahead of the server, and a change of meaning keeps the layout.
    std::uint32_t format_version = 0;

    /// Milliseconds since epoch, sampled before the build started.
    std::int64_t built_at = 0;

    /// The include tree (IncludeTree's persisted vectors): the path table,
    /// the consumed-content hash per path, and every include edge of the
    /// parse.
    Seq<Seq<char>> paths;
    Seq<std::uint64_t> content_hashes;
    Seq<IncludeNode> nodes;

    /// The symbols the entries name, sorted by hash, except the file-local
    /// ones (a function's locals, a template's parameters) — only their
    /// own file can mention one, and that file's section names it — and
    /// the external symbols only sections left empty name. Names, arguments and signatures are
    /// back to back with one end offset per entry; so are the reference files, as path ids.
    Seq<std::uint64_t> symbol_hashes;
    Seq<char> symbol_names;
    Seq<std::uint32_t> symbol_name_ends;
    Seq<char> symbol_args;
    Seq<std::uint32_t> symbol_args_ends;
    Seq<char> symbol_signatures;
    Seq<std::uint32_t> symbol_signature_ends;
    Seq<std::uint64_t> symbol_parents;
    Seq<std::uint8_t> symbol_kinds;
    Seq<std::uint8_t> symbol_scopes;
    Seq<std::uint16_t> symbol_flags;
    Seq<std::uint8_t> symbol_token_kinds;
    Seq<std::uint32_t> symbol_token_modifiers;
    /// The path id of a file declaring each (SymbolView::file), `no_file` for
    /// a symbol without a declaring relation.
    Seq<std::uint32_t> symbol_files;
    Seq<std::uint32_t> symbol_reference_ends;
    Seq<std::uint32_t> symbol_references;

    /// Per symbol, the files whose instantiation section names it, back to
    /// back as path ids.
    Seq<std::uint32_t> symbol_instantiation_ends;
    Seq<std::uint32_t> symbol_instantiations;

    /// One entry per file with an index, ascending by path id.
    Seq<FileSectionOf<Seq>> sections;

    /// One entry per file the run's instantiations or macro expansions
    /// resolve something in or skipped a known key of, ascending by path id.
    Seq<InstantiationSectionOf<Seq>> instantiations;

    /// The places the parse's failed lookups looked that held no file
    /// (CompilationUnit::absent).
    Seq<Seq<char>> absent;
};

using TUIndexBlob = TUIndexBlobOf<codec::blob::Owned>;

}  // namespace clice::index

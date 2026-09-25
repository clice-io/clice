#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "index/manifest.h"
#include "index/project_index.h"
#include "index/site.h"
#include "index/symbol_query.h"
#include "index/tu_index.h"
#include "index/types.h"
#include "semantic/symbol.h"
#include "vfs/file_table.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/MemoryBuffer.h"

namespace clice::index {

/// One readable row set with its own coordinate system, as the federation
/// hands it to a visitor.
struct RowSource {
    enum class Kind : std::uint8_t {
        /// A persisted shard, in the coordinates of the text it indexed —
        /// or of an open buffer byte-identical to that text.
        Shard,
        /// An open buffer's own file index, in buffer coordinates.
        SessionRows,
        /// An open buffer's preamble region as its PCH overlay indexed it,
        /// in buffer coordinates.
        PreambleRows,
        /// A header entry of a PCH overlay, in the coordinates of the text
        /// the overlay indexed.
        Overlay,
    };

    Kind kind;
    Fid file;
    llvm::StringRef path;
    const Shard* rows;
    Coordinates coords;

    /// The site of a row's range; nullopt for a range outside the text.
    std::optional<Site> site(LocalSourceRange range) const {
        auto begin = coords.position(range.begin);
        auto end = coords.position(range.end);
        if(!begin || !end) {
            return std::nullopt;
        }
        return Site{.file = file, .path = path, .range = range, .begin = *begin, .end = *end};
    }
};

/// What a server adds to the persisted index: the open buffers' own rows,
/// the preamble regions their PCH overlays indexed, and the overlays'
/// header entries. Every row source it hands out is read synchronously
/// within the visit.
class LiveSources {
public:
    virtual ~LiveSources() = default;

    /// Whether the file is an open buffer. An open buffer is served by
    /// claim() or by nothing at all — never by its shard as a closed file.
    virtual bool is_open(Fid file) const = 0;

    /// The rows serving an open buffer right now: its own file index
    /// (freshness clauses 1 and 3), else its shard while the buffer is
    /// byte-identical to the text the shard indexed (clause 4), both in
    /// buffer coordinates. Nullopt while the buffer has moved on from both.
    virtual std::optional<RowSource> claim(Fid file) const = 0;

    /// Every open buffer whose file index serves it (clause 3), as its rows.
    virtual void each_session(llvm::function_ref<bool(const RowSource&)> visit) const = 0;

    /// The same buffers' index envelopes: their symbol tables know every
    /// symbol of the unsaved text.
    virtual void each_session_index(llvm::function_ref<bool(const TUIndex&)> visit) const = 0;

    /// Every open buffer's preamble rows whose overlay was built from that
    /// very file and still describes the buffer's start.
    virtual void each_preamble(llvm::function_ref<bool(const RowSource&)> visit) const = 0;

    /// Each distinct PCH overlay envelope once (buffers sharing a preamble
    /// share one).
    virtual void each_overlay(llvm::function_ref<bool(const TUIndex&)> visit) const = 0;

    /// Whether an overlay header entry must never reach the user: the
    /// server's own synthesized context artifacts.
    virtual bool excluded(llvm::StringRef path) const = 0;

    /// An open buffer's PCH envelope while the buffer still starts with
    /// the exact preamble the envelope was built from; null otherwise.
    virtual std::shared_ptr<TUIndex> preamble_blob(Fid file) const = 0;
};

struct FreshnessOptions {
    /// Look at each file once per gate instead of trusting the last
    /// observation: a reader nobody keeps the observations current for
    /// (the command line), whose gate lives one query.
    bool look = false;

    /// Rows no indexer will ever refresh keep serving instead of leaving
    /// a permanent hole: off when background indexing is.
    bool withhold = true;
};

/// Freshness clause 2: whether rows built from `content_hash` no longer
/// describe the file's content on disk — they would point at text that no
/// longer exists. The one question every disk-side row source is judged by
/// (persisted shards, PCH overlay entries), against what the file table
/// last saw on disk. A file seen missing keeps its last-known rows: they
/// are the only remaining truth about it.
class FreshnessGate {
public:
    explicit FreshnessGate(FileTable& files, FreshnessOptions options = {}) :
        options(options), files(files) {}

    bool stale(Fid file, std::uint64_t content_hash) const;

    /// The files whose rows were withheld, in no particular order.
    llvm::SmallVector<Fid> withheld() const {
        return llvm::to_vector(withheld_files);
    }

    FreshnessOptions options;

private:
    FileTable& files;
    mutable llvm::DenseSet<Fid> looked;
    mutable llvm::DenseSet<Fid> withheld_files;
};

/// Cross-source dedup: a row present in both a disk shard and a PCH
/// overlay (or in two overlays sharing a preamble, or in two projects'
/// indexes) comes out identical.
void dedup_sites(std::vector<Site>& sites);

/// Read-only queries over every index source: disk shards, open sessions'
/// file indexes, PCH overlays and the buffers' own preamble rows. Holds no
/// index data of its own — ProjectIndex owns the disk-derived index, the
/// live sources the per-buffer indexes.
///
/// Every answer is a domain value (Site, SymbolRef); the transports render
/// them. Every method is synchronous and its results borrow the state they
/// were read from — consume them before the next suspension point.
///
/// Freshness contract — results may be incomplete, by design:
///
///   1. Cursor resolution (turning an offset in the request's file into a
///      symbol) is accurate: callers that hold an open session await its
///      compile first, so the session's file index describes the buffer
///      being pointed at. For closed files the merged shard resolves
///      against its own stored content snapshot — unless the file's own
///      content moved on from it, in which case the cursor is
///      unresolvable (clause 2).
///   2. Every row source carries the hash of the text it indexed, and
///      serves only while that is the file's current content: disk rows
///      (shards, PCH overlay entries) while the disk still holds it (the
///      freshness gate), an open buffer's rows while the buffer does.
///      Rows whose dependencies changed but whose own text did not keep
///      serving — positionally intact, at worst semantically behind — and
///      rows of text that no longer exists never do.
///   3. An open session's own file index serves under clause 2 against
///      the buffer: while its compile is current, or when it compiled the
///      very bytes the buffer holds (a dependency invalidated it, the
///      buffer did not move). Mid-edit it is skipped: unlike closed files
///      its reindex is the next compile, which the current file's request
///      already awaits.
///   4. An open session without a current file index is served by the
///      file's shard under closed-file rules — but only while the buffer
///      is byte-identical to the content the rows were built from. This
///      is what serves documents opened for reading before any compile is
///      invested in them; the moment the buffer diverges the shard
///      withdraws and clauses 1-3 govern again. Arbitration is strict: a
///      current file index always wins over the shard (never both).
///
///   Symbol identity lookups (symbol_info: hash → name/kind) are not
///   gated: a hash identifies one symbol, so even a stale shard answers
///   them correctly.
class IndexQuery {
public:
    /// A null gate never withholds; null live sources are the disk-only
    /// view of headless tools: every file answers as if closed.
    IndexQuery(const ProjectIndex& index,
               const FileTable& files,
               const FreshnessGate* gate,
               const LiveSources* live);

    /// The freshness contract's single arbitration: the rows serving
    /// `file` right now, with the coordinates they are expressed in.
    std::optional<RowSource> serving(Fid file) const;

    /// Whether the project index holds rows of `file`.
    bool indexes(Fid file) const {
        return index.shards.contains(file);
    }

    /// The file's shard when `text` is byte-identical to the content it
    /// indexed (clause 4's content gate alone): disk-truth products such as
    /// manifest edges are only meaningful against text the index described.
    const Shard* shard_matching(Fid file, llvm::StringRef text) const;

    /// The live sources' PCH envelope for an open buffer (see
    /// LiveSources::preamble_blob); null without live sources.
    std::shared_ptr<TUIndex> preamble_blob(Fid file) const;

    /// The symbol whose occurrence covers `offset` in the file's serving
    /// source (clauses 1 and 4), and the site of that occurrence. A session
    /// served by its own rows also resolves through the preamble region
    /// of its PCH overlay — compiled into the PCH, invisible to the
    /// per-edit index, spelled in the same buffer coordinates.
    struct Cursor {
        SymbolHash symbol = 0;
        Site site;
    };

    std::optional<Cursor> symbol_at(Fid file, std::uint32_t offset) const;

    /// The same for a position as the editor spells it: a line and a
    /// UTF-16 column in the serving source's text.
    std::optional<Cursor> symbol_at(Fid file, std::uint32_t line, std::uint32_t utf16_column) const;

    /// A symbol's name and kind, from whichever table knows the hash: open
    /// sessions, the project index, PCH overlays, then the per-file shards
    /// (TU-local names live only there).
    std::optional<SymbolRef> symbol_info(SymbolHash hash) const;

    /// The containers of a symbol, outermost first: the parent chain up
    /// to the translation unit or to a parent no table knows, inline
    /// namespaces skipped (anonymous ones never are parents). Empty at the
    /// translation unit and for an unknown hash.
    llvm::SmallVector<SymbolRef, 4> container_chain(SymbolHash hash) const;

    /// The chain spelled as a qualified name ("ns::Outer" for
    /// `ns::Outer::name`).
    std::string container_name(SymbolHash hash) const;

    /// The symbol's name qualified by its container, a specialization's
    /// arguments included ("ns::Outer::name<int>"). Empty for an unknown
    /// hash.
    std::string qualified_name(SymbolHash hash) const;

    /// Every site carrying a relation of `kind` for the symbol, across all
    /// serving sources, deduplicated — a row present in both a disk shard
    /// and an overlay comes out identical.
    std::vector<Site> sites(SymbolHash hash, RelationKind kind) const;

    /// The first site carrying the relation, live sources first: an open
    /// buffer's rows, its preamble region, PCH overlays (the definition as
    /// seen under the live context — present even when no disk TU was
    /// indexed), then disk shards.
    std::optional<Site> first_site(SymbolHash hash, RelationKind kind) const;

    /// The symbol's canonical site: its definition, or a declaration when
    /// nothing defines it (pure virtuals, externs, decl-only APIs).
    std::optional<Site> canonical_site(SymbolHash hash) const;

    /// Go-to-definition from a cursor: the definition sites, or — standing
    /// on the definition itself, or when nothing defines the symbol — the
    /// declarations (and sibling definitions), so definition and
    /// declaration sites alternate. The cursor's own site drops out unless
    /// it is the only site the symbol has.
    std::vector<Site> definition(const Cursor& cursor) const;

    /// The mirror half of definition's alternation: declarations plus the
    /// definition, minus the site the cursor stands on.
    std::vector<Site> declaration(const Cursor& cursor) const;

    /// The references of the symbol under the cursor, optionally folding in
    /// its declarations and definitions, deduplicated across the kinds —
    /// rows of different kinds can share one anchor.
    std::vector<Site> references(const Cursor& cursor, bool include_declaration) const;

    /// One canonical site per distinct relation target — the two-hop query
    /// behind go-to-type-definition.
    std::vector<Site> target_sites(SymbolHash hash, RelationKind kind) const;

    /// Sites implementing the symbol: derived types for a class-like
    /// symbol, override targets otherwise.
    std::vector<Site> implementation(SymbolHash hash) const;

    /// A symbol's definition as text: the extent's site, the text it
    /// spans and the comment block above it, sliced from the first source
    /// that holds the definition in first-hit order (an open buffer's
    /// text, its preamble region, an overlay's stored text or the disk
    /// re-read for pure-ASCII blobs — verified against the rows' content
    /// hash, so a moved-on file degrades to no text rather than mismatched
    /// text).
    struct Definition {
        Site extent;
        std::string text;
        std::string comment;
    };

    std::optional<Definition> definition_text(SymbolHash hash) const;

    /// The source line a site lies on, for previews; empty when the text
    /// is unavailable (see definition_text on the disk re-read).
    std::string context_line(const Site& site) const;

    /// A symbol together with its canonical site.
    struct Located {
        SymbolRef symbol;
        Site site;
    };

    std::optional<Located> resolve(SymbolHash hash) const;

    /// One neighbour of a symbol in a graph: the symbol at its canonical
    /// site and the sites of the relation rows that connect them.
    struct Edge {
        Located symbol;
        std::vector<Site> sites;
    };

    /// The functions calling `root` and the ones it calls, with the call
    /// sites; only the sides asked for are walked. A neighbour no source
    /// places is left out.
    struct CallGraphOptions {
        bool callers = true;
        bool callees = true;
    };

    struct CallGraph {
        std::vector<Edge> callers;
        std::vector<Edge> callees;
    };

    CallGraph call_graph(SymbolHash root, CallGraphOptions options) const;

    /// The types `root` derives from and the ones deriving from it, each
    /// at its canonical site; only the sides asked for are walked.
    struct TypeHierarchyOptions {
        bool supertypes = true;
        bool subtypes = true;
    };

    struct TypeHierarchy {
        std::vector<Located> supertypes;
        std::vector<Located> subtypes;
    };

    TypeHierarchy type_hierarchy(SymbolHash root, TypeHierarchyOptions options) const;

    /// The symbols a name query (index/symbol_query.h) matches, best
    /// first, at most `limit`: the search index's hits, the symbols merged
    /// since it was built and the open sessions' own symbols — scanned
    /// directly — under one ranking, each with its canonical site. A
    /// symbol no source places is left out. Empty for a query by id or
    /// place.
    std::vector<Located> search(const SymbolQuery& query, std::size_t limit) const;

    /// The symbols a locator query names: by id; by place — the symbol
    /// under a cursor, or those defined on a line of the file's serving
    /// source; or by pattern, where the symbols spelling the name exactly
    /// (in any case) are the answer when there are any, and the ranked
    /// matches stand as candidates otherwise.
    std::vector<Located> locate(const SymbolQuery& query) const;

    /// Every project symbol with a definition site in the file's serving
    /// source, anchored at the definition's name token.
    std::vector<Located> definitions_in(Fid file) const;

    /// The include edges of a document, the input of the document-link
    /// projection: from its own TU manifest when it has one, else from the
    /// contributing TUs' manifests (a header reached only through source
    /// TUs — its directives are nodes hanging off the header's own node
    /// there). Only manifests that entered the document at its shard's
    /// content generation contribute; empty when the file was never
    /// indexed.
    std::vector<IncludeEdge> include_edges(Fid file) const;

private:
    /// Which sources a relation walk visits and in what order. LiveFirst
    /// is the first-hit order (session rows, preamble rows, overlays,
    /// disk); DiskFirst the collect-all order.
    enum class Order : std::uint8_t { LiveFirst, DiskFirst };

    struct SourceMask {
        bool shard = true;
        bool session = true;
        bool preamble = true;
        bool overlay = true;
    };

    using RelationVisitor = llvm::function_ref<bool(const RowSource&, const Relation&)>;

    /// A search's ranked symbols before their sites are resolved, and
    /// whether the limit cut the ranking short.
    struct Ranked {
        NameRank rank;
        SymbolRef symbol;
    };

    struct RankedHits {
        std::vector<Ranked> hits;
        bool exhausted = true;
    };

    RankedHits ranked_search(const SymbolQuery& query, std::size_t limit) const;

    /// Relations of `kind` grouped by their target symbol, each with the
    /// sites spelling it, the targets resolved to their canonical sites.
    std::vector<Edge> edges(SymbolHash hash, RelationKind kind) const;

    /// The distinct target symbols of the symbol's relations of `kind`
    /// (bases, derived types, overrides), in first-seen order.
    llvm::SmallVector<SymbolHash> targets(SymbolHash hash, RelationKind kind) const;

    /// One canonical site per distinct relation target.
    std::vector<Located> located_targets(SymbolHash hash, RelationKind kind) const;

    /// The one federation walk every relation query is a fold over. The
    /// visitor returns false to stop.
    void for_each_relation(SymbolHash hash,
                           RelationKind kind,
                           Order order,
                           SourceMask mask,
                           RelationVisitor visitor) const;

    /// The header entries of an overlay that may contribute results:
    /// files that are themselves open serve buffer-true rows through their
    /// sessions, entries of text the disk no longer holds point nowhere
    /// (clause 2), and synthesized context artifacts must never send the
    /// user into the cache.
    void visit_overlay_files(const TUIndex& state,
                             llvm::function_ref<bool(const RowSource&)> visitor) const;

    /// The text a source's offsets index: the buffer, the blob's stored
    /// text, or the disk re-read for pure-ASCII blobs (kept alive in
    /// `storage`), verified against the rows' content hash.
    std::optional<llvm::StringRef> source_text(const RowSource& source,
                                               std::unique_ptr<llvm::MemoryBuffer>& storage) const;

    const ProjectIndex& index;
    const FileTable& files;
    const FreshnessGate* gate;
    const LiveSources* live;
};

}  // namespace clice::index

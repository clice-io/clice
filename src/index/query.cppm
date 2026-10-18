module;

#include "modules/prelude.h"

module clice:index.query;

import :index.instantiation_shard;
import :index.project_index;
import :index.site;
import :index.symbol;
import :index.symbol_query;
import :index.tu_index;
import :index.tu_manifest;
import :semantic.symbol;
import :syntax.position;
import :vfs.file_table;

namespace clice::index {

/// One readable index with its own coordinate system, as the federation
/// hands it to a visitor.
struct IndexSource {
    enum class Kind : std::uint8_t {
        /// A persisted shard, in the coordinates of the text it indexed —
        /// or of an open buffer byte-identical to that text.
        Shard,
        /// An open buffer's own file index, in buffer coordinates.
        SessionShard,
        /// An open buffer's preamble region as its PCH overlay indexed it,
        /// in buffer coordinates.
        PreambleShard,
        /// A header entry of a PCH overlay, or an imported module's
        /// declarations an open buffer's index holds, in the coordinates of
        /// the text the index read.
        Overlay,
    };

    Kind kind;
    Fid file;
    std::string path;
    const Shard* shard;
    PositionMap positions;

    /// What the instantiations and macro expansions of the text resolve,
    /// beside the shard: an open buffer's own compile's, and the stored
    /// ones while the buffer is the stored text; empty when none serves.
    llvm::SmallVector<const InstantiationShard*, 2> instantiations;

    /// What the entries handed out with the source come from: the shard's
    /// are written, `instantiations`' records are not.
    SiteOrigin origin = SiteOrigin::Written;

    /// The site of an entry's range; nullopt for a range outside the text.
    std::optional<Site> site(LocalSourceRange range) const {
        if(range.end > positions.size()) {
            return std::nullopt;
        }
        return Site{
            .file = file,
            .path = path,
            .range = range,
            .begin = *line_column(positions, range.begin),
            .end = *line_column(positions, range.end),
            .origin = origin,
        };
    }
};

/// What a server adds to the persisted index: the open buffers' own indexes,
/// the preamble regions their PCH overlays indexed, and the overlays'
/// header entries. Every index source it hands out is read synchronously
/// within the visit.
class LiveSources {
public:
    virtual ~LiveSources() = default;

    /// Whether the file is an open buffer. An open buffer is served by
    /// buffer_index() or by nothing at all — never by its shard as a closed file.
    virtual bool is_open(Fid file) const = 0;

    /// The index serving an open buffer right now: its own file index
    /// (freshness clauses 1 and 3), else its shard while the buffer is
    /// byte-identical to the text the shard indexed (clause 4), both in
    /// buffer coordinates. Nullopt while the buffer has moved on from both.
    virtual std::optional<IndexSource> buffer_index(Fid file) const = 0;

    /// Every open buffer whose file index serves it (clause 3), as its index.
    virtual void each_session(llvm::function_ref<bool(const IndexSource&)> visit) const = 0;

    /// The same buffers' TU indexes, which name (find_symbol) every
    /// symbol of the unsaved text and hold the declarations of what it
    /// names from imported modules.
    virtual void each_session_index(llvm::function_ref<bool(const TUIndex&)> visit) const = 0;

    /// Every open buffer's preamble index whose overlay was built from that
    /// very file and still describes the buffer's start.
    virtual void each_preamble(llvm::function_ref<bool(const IndexSource&)> visit) const = 0;

    /// Each distinct PCH overlay index once (buffers sharing a preamble
    /// share one).
    virtual void each_overlay(llvm::function_ref<bool(const TUIndex&)> visit) const = 0;

    /// An open buffer's PCH index while the buffer still starts with
    /// the exact preamble the index was built from; null otherwise.
    virtual std::shared_ptr<TUIndex> preamble_blob(Fid file) const = 0;
};

struct FreshnessOptions {
    /// Check each file on disk once per gate instead of trusting the last
    /// observation: a reader nobody keeps the observations current for
    /// (the command line), whose gate lives one query.
    bool check_disk = false;

    /// Indexes no indexer will ever refresh keep serving instead of leaving
    /// a permanent hole: off when background indexing is.
    bool withhold = true;
};

/// Freshness clause 2: whether an index built from `content_hash` no longer
/// describes the file's content on disk — it would point at text that no
/// longer exists. The one question every disk-side index source is judged by
/// (persisted shards, PCH overlay entries), against what the file table
/// last saw on disk. A file seen missing serves nothing: its index describes
/// text that is gone with it.
class FreshnessGate {
public:
    explicit FreshnessGate(FileTable& files, FreshnessOptions options = {}) :
        options(options), files(files) {}

    bool stale(Fid file, std::uint64_t content_hash) const;

    /// The files whose indexes were withheld, in no particular order.
    llvm::SmallVector<Fid> withheld() const {
        return llvm::to_vector(withheld_files);
    }

    FreshnessOptions options;

private:
    FileTable& files;
    mutable llvm::DenseSet<Fid> checked;
    mutable llvm::DenseSet<Fid> withheld_files;
};

/// Whether the shard declares or defines the symbol, not only names it.
bool declares(const Shard& shard, SymbolHash hash);

/// Cross-source dedup: an entry present in both a disk shard and a PCH
/// overlay (or in two overlays sharing a preamble, or in two projects'
/// indexes) comes out identical; one an instantiation resolves where the
/// written code says the same comes out as written.
void dedup_sites(std::vector<Site>& sites);

/// The relations of `kind` for `symbol` the live records of an
/// instantiation shard make (InstantiationKind says which), each with
/// whether a macro expansion resolves it; false once the visitor stopped.
bool resolved_relations(const InstantiationShard& shard,
                        SymbolHash symbol,
                        RelationKind kind,
                        llvm::function_ref<bool(const Relation&, bool expansion)> visit);

/// Whether a relation's site spells the name a cursor stands on: a relation spans
/// the whole written name, an occurrence only the tokens the name owns
/// (`operator` of `operator Foo*`, whose `Foo` is the class's).
bool covers(const Site& site, const Site& cursor);

struct Named {
    LocalSourceRange range;
    llvm::SmallVector<SymbolHash, 1> symbols;
};

/// The symbols one shard names at `offset`, and the range naming them:
/// the occurrences sharing the first range the lookup meets. A module
/// imported through a macro sits under the macro's own occurrence — the
/// name spells what the macro expanded to, and the macro itself is
/// reached at its definition — so a macro gives way to the symbols it
/// shares its range with.
std::optional<Named> named_at(const Shard& shard,
                              std::uint32_t offset,
                              llvm::function_ref<bool(SymbolHash)> is_macro);

/// Adds to `named` what an instantiation or a macro expansion resolves at
/// `offset`: a name its template leaves open (a member of a dependent
/// object), or more of what the name means.
void add_resolved(const InstantiationShard& shard,
                  std::uint32_t offset,
                  std::optional<Named>& named);

/// How the code at a highlighted name accesses its symbol.
enum class HighlightKind : std::uint8_t {
    /// A declaration or definition, a use of a symbol nothing reads or
    /// writes, or a use only an instantiation or a macro expansion
    /// resolves, which records no access.
    Text,
    Read,
    Write,
};

struct Highlight {
    LocalSourceRange range;
    HighlightKind kind;
};

/// The relations of `symbols` in the shards of one text, and the uses of
/// them the text's instantiations and macro expansions resolve, one
/// highlight per range in range order: a declaration or definition Text; a
/// reference of a variable, field or parameter Read, a Write relation Write;
/// a reference of any other symbol, or a use only an instantiation or a
/// macro expansion resolves, Text — the strongest of them where several
/// share a range. A class's name also highlights where its constructors
/// are declared. `kind_of` is a symbol's kind, when known.
std::vector<Highlight>
    highlights(llvm::ArrayRef<const Shard*> shards,
               llvm::ArrayRef<const InstantiationShard*> instantiations,
               llvm::ArrayRef<SymbolHash> symbols,
               llvm::function_ref<std::optional<SymbolKind>(SymbolHash)> kind_of);

/// Read-only queries over every index source: disk shards, open sessions'
/// file indexes, PCH overlays and the buffers' own preamble indexes. Holds no
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
///   2. Every index source carries the hash of the text it indexed, and
///      serves only while that is the file's current content: disk indexes
///      (shards, PCH overlay entries) while the disk still holds it (the
///      freshness gate), an open buffer's index while the buffer does.
///      Indexes whose dependencies changed but whose own text did not keep
///      serving — positionally intact, at worst semantically behind — and
///      indexes of text that no longer exists never do.
///   3. An open session's own file index serves under clause 2 against
///      the buffer: while its compile is current, or when it compiled the
///      very bytes the buffer holds (a dependency invalidated it, the
///      buffer did not move). Mid-edit it is skipped: unlike closed files
///      its reindex is the next compile, which the current file's request
///      already awaits.
///   4. An open session without a current file index is served by the
///      file's shard under closed-file rules — but only while the buffer
///      is byte-identical to the content the shard was built from. This
///      is what serves documents opened for reading before any compile is
///      invested in them; the moment the buffer diverges the shard
///      withdraws and clauses 1-3 govern again. Arbitration is strict: a
///      current file index always wins over the shard (never both).
///
///   Symbol identity lookups (symbol_info: hash → name/kind) are not
///   gated: a hash identifies one symbol, so even a stale shard answers
///   them correctly.
///
/// The project table lists the files holding an external symbol's entries,
/// and only an external one's: every question about a symbol's entries also
/// takes an `anchor`, a file holding one of them — the cursor's file, the
/// file a relation was read from, a located symbol's site. Any other
/// symbol's entries are the anchor's and those in the files the TUs' local
/// fanouts list for it (ProjectIndex::each_fanout_file). External symbols
/// ignore the anchor.
class IndexQuery {
public:
    /// A null gate never withholds; null live sources are the disk-only
    /// view of headless tools: every file answers as if closed.
    IndexQuery(const ProjectIndex& index,
               FileTable& files,
               const FreshnessGate* gate,
               const LiveSources* live);

    /// The same queries over the written code alone, without what
    /// instantiations and macro expansions resolve: a rename edits names
    /// as the text spells them.
    IndexQuery written_only() const {
        auto query = *this;
        query.resolved = false;
        return query;
    }

    /// The freshness contract's single arbitration: the index serving
    /// `file` right now, with the coordinates it is expressed in.
    std::optional<IndexSource> serving(Fid file) const;

    /// Whether the project index holds a shard of `file`.
    bool indexes(Fid file) const {
        return index.shards.contains(file);
    }

    /// The file's shard when `text` is byte-identical to the content it
    /// indexed (clause 4's content gate alone): disk-truth products such as
    /// manifest edges are only meaningful against text the index described.
    const Shard* shard_matching(Fid file, llvm::StringRef text) const;

    /// The live sources' PCH index for an open buffer (see
    /// LiveSources::preamble_blob); null without live sources.
    std::shared_ptr<TUIndex> preamble_blob(Fid file) const;

    /// The symbols whose occurrences cover `offset` in the file's serving
    /// source (clauses 1 and 4), and the site of that occurrence. A session
    /// served by its own index also resolves through the preamble region
    /// of its PCH overlay — compiled into the PCH, invisible to the
    /// per-edit index, spelled in the same buffer coordinates.
    ///
    /// One name can spell several symbols: an overload set a template's
    /// call leaves open, `using Base::Base` naming each inherited
    /// constructor, the variants of a shared header naming different
    /// entities. The questions about a cursor answer for all of them.
    struct Cursor {
        llvm::SmallVector<SymbolHash, 1> symbols;
        Site site;
    };

    std::optional<Cursor> symbol_at(Fid file, std::uint32_t offset) const;

    /// The same for a position as the editor spells it: a line and a
    /// UTF-16 column in the serving source's text.
    std::optional<Cursor> symbol_at(Fid file, std::uint32_t line, std::uint32_t utf16_column) const;

    /// A symbol's name and kind, from whichever table knows the hash: open
    /// sessions, the project index, PCH overlays, then the per-file shards
    /// (names below external scope live only there), the anchor's first.
    std::optional<SymbolRef> symbol_info(SymbolHash hash, Fid anchor = {}) const;

    /// The containers of a symbol, outermost first: the parent chain up
    /// to the translation unit or to a parent no table knows, transparent
    /// scopes skipped (anonymous namespaces never are parents). Empty at
    /// the translation unit and for an unknown hash. `anchor` as for
    /// symbol_info: a symbol of internal linkage and its containers are
    /// known to the files holding its entries.
    llvm::SmallVector<SymbolRef, 4> container_chain(SymbolHash hash, Fid anchor = {}) const;

    /// The chain spelled as a qualified name ("ns::Outer" for
    /// `ns::Outer::name`).
    std::string container_name(SymbolHash hash, Fid anchor = {}) const;

    /// The symbol's name qualified by its container, a specialization's
    /// arguments included ("ns::Outer::name<int>"). Empty for an unknown
    /// hash.
    std::string qualified_name(SymbolHash hash, Fid anchor = {}) const;

    /// Every site carrying a relation of `kind` for the symbol, across all
    /// serving sources, deduplicated — a relation present in both a disk shard
    /// and an overlay comes out identical.
    std::vector<Site> sites(SymbolHash hash, Fid anchor, RelationKind kind) const;

    /// The first site carrying the relation, live sources first: an open
    /// buffer's index, its preamble region, PCH overlays (the definition as
    /// seen under the live context — present even when no disk TU was
    /// indexed), then disk shards.
    std::optional<Site> first_site(SymbolHash hash, Fid anchor, RelationKind kind) const;

    /// The shard saying which symbols a file declares and defines, for a
    /// question positions play no part in: the index serving the file, else
    /// for a buffer edited since its last compile the shard of its disk
    /// text — the edit moved positions, not most of what the file declares.
    /// Null for a closed file whose shard the freshness gate withholds.
    const Shard* declaring_shard(Fid file) const;

    /// The open buffers whose own index carries a relation of `kind` for the
    /// symbol, each once: what a buffer its own compile serves knows before
    /// the project table does — the table hears of the buffer only once it
    /// closes.
    void each_live_file(SymbolHash hash,
                        RelationKind kind,
                        llvm::function_ref<void(Fid)> visit) const;

    /// The file defining an external symbol: an open buffer defining it
    /// (each_live_file) first, else the definition the project table
    /// records, while that file's shard still holds it. Invalid when neither
    /// knows one; of several files defining the symbol, one. Never walks
    /// the files referencing the symbol, so it costs the same for a symbol
    /// every file uses.
    Fid definition_file(SymbolHash hash) const;

    /// The definitions go-to-definition lists for a symbol: all of them,
    /// but of a namespace, which each of its blocks defines, the canonical
    /// one.
    std::vector<Site> definition_sites(SymbolHash hash, Fid anchor) const;

    /// Go-to-definition from a cursor: the definition sites (at its own
    /// declaration, a virtual function nothing defines is defined by its
    /// overrides), or —
    /// standing on the definition itself, or when nothing defines the
    /// symbol — the declarations (and sibling definitions) and, at a
    /// specialization, the template it specializes, so definition and
    /// declaration sites alternate; a namespace's blocks never do. The
    /// cursor's own site drops out unless it is the only site the symbol
    /// has.
    std::vector<Site> definition(const Cursor& cursor) const;

    /// The mirror half of definition's alternation: declarations plus the
    /// definition, minus the site the cursor stands on. At a function
    /// defined with no declaration of its own, the declarations an edit to
    /// one side may have parted from it (mismatched_declarations).
    std::vector<Site> declaration(const Cursor& cursor) const;

    /// The references of the symbol under the cursor, weak ones included (a
    /// template's call through an overload set or a dependent name, which
    /// names its candidates only heuristically), optionally folding in its
    /// declarations and definitions, deduplicated across the kinds — relations
    /// of different kinds can share one anchor. A method's include the
    /// references of the methods it overrides, a call through which may
    /// dispatch to it, but not one naming that method by qualification
    /// (`Base::f()`); a class's the names of its constructors.
    std::vector<Site> references(const Cursor& cursor, bool include_declaration) const;

    /// The qualified name of the function whose definition holds each site,
    /// the innermost; empty where none does. A function's own name lies in
    /// none.
    std::vector<std::string> enclosing_functions(llvm::ArrayRef<Site> sites) const;

    /// A function some source defines: its definition's extent and its name.
    struct DefinedFunction {
        SymbolHash hash;
        LocalSourceRange extent;
        LocalSourceRange name;
    };

    struct PlacedHighlight {
        Site site;
        HighlightKind kind;
    };

    /// The highlights of the symbols under the cursor in the cursor's own
    /// file, from the index serving it — a session's preamble region
    /// included.
    std::vector<PlacedHighlight> document_highlights(const Cursor& cursor) const;

    /// Go-to-type-definition of a cursor's symbol: the declarations its
    /// type names — at a name of a member of a template's specialization,
    /// the type the specialization gives it (TypeDefinition relations
    /// spanning the cursor in the index serving its file); at a type, the
    /// type itself; elsewhere the type it is declared with (the relations
    /// without a range). An alias leads on to the type it names. The
    /// cursor's own site drops out unless it is the only one.
    std::vector<Site> type_definition(const Cursor& cursor, SymbolHash symbol) const;

    /// Whether the name at the cursor may mean `symbol` without naming it
    /// for certain — a dependent name, an overload set's candidate, a
    /// using-declaration's target: a WeakReference relation at the
    /// cursor's name in the index serving its file.
    bool names_weakly(const Cursor& cursor, SymbolHash symbol) const;

    /// A symbol's definition as text: the extent's site and the text it
    /// spans, sliced from the first source that holds the definition in
    /// first-hit order (an open buffer's text, its preamble region, or the
    /// file read from disk — verified against the index's content hash, so
    /// a moved-on file degrades to no text rather than mismatched text).
    struct DeclaredText {
        Site extent;
        std::string text;
    };

    std::optional<DeclaredText> definition_text(SymbolHash hash, Fid anchor) const;

    /// The same of a declaration.
    std::optional<DeclaredText> declaration_text(SymbolHash hash, Fid anchor) const;

    /// The text (comment_text) of the comment documenting the symbol, read
    /// as the compiled unit's doc_comment reads it: of its declarations,
    /// then its definitions, in first-hit order, the first a Doxygen comment
    /// documents, else the first any comment documents. With `inherit`, a
    /// method overriding another without one takes that method's.
    std::string doc_comment(SymbolHash hash, Fid anchor, bool inherit) const;

    /// The source line each site lies on, for previews, each file read
    /// once; empty when the text is unavailable (see definition_text on the
    /// disk re-read).
    std::vector<std::string> context_lines(llvm::ArrayRef<Site> sites) const;

    /// The text the index serving `file` was built from — the offsets of
    /// every site in it index this text; nullopt when no source serves the
    /// file or its text is unavailable (see definition_text on the disk
    /// re-read).
    std::optional<std::string> serving_text(Fid file) const;

    /// A symbol together with its canonical site, and the whole
    /// declaration there.
    struct Located {
        SymbolRef symbol;
        Site site;
        Site extent;
    };

    std::optional<Located> resolve(SymbolHash hash, Fid anchor) const;

    /// The symbols a `@copydoc` in the comment of the symbol may name
    /// (parse_copied_name), as the compiled unit's copied_doc_targets finds
    /// them: from the symbol's scope outward, or in the global scope; of
    /// several, the functions declaring the parameters it lists, when it
    /// lists them.
    std::vector<Located> copied_doc_targets(SymbolHash hash,
                                            Fid anchor,
                                            llvm::StringRef name) const;

    /// The symbols under a cursor, each with its canonical site; one no
    /// source places is left out.
    std::vector<Located> resolve_at(const Cursor& cursor) const;

    /// What implements the symbol, each at its canonical site: the types
    /// deriving from a class-like symbol, the overrides of a function, at
    /// any depth; for a function nothing overrides, its definitions.
    std::vector<Located> implementation(SymbolHash hash, Fid anchor) const;

    /// The distinct targets of the symbol's relations of `kind` (bases,
    /// overrides, constructors, specializations), each at its canonical
    /// site; a target no source places is left out.
    std::vector<Located> located_targets(SymbolHash hash, Fid anchor, RelationKind kind) const;

    /// One neighbour of a symbol in a graph: the symbol at its canonical
    /// site and the sites of the relations that connect them.
    struct Edge {
        Located symbol;
        std::vector<Site> sites;
    };

    /// The functions calling `root` and the ones it calls, with the call
    /// sites; only the sides asked for are walked. A method's callers
    /// include those of the methods it overrides, whose calls may dispatch
    /// to it, but not one naming that method by qualification. A
    /// variable, a field or an enumerator has as callers the functions
    /// using it, a function's local none. A neighbour no source places is
    /// left out.
    struct CallGraphOptions {
        bool callers = true;
        bool callees = true;
    };

    struct CallGraph {
        std::vector<Edge> callers;
        std::vector<Edge> callees;
    };

    CallGraph call_graph(SymbolHash root, Fid anchor, CallGraphOptions options) const;

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

    TypeHierarchy type_hierarchy(SymbolHash root, Fid anchor, TypeHierarchyOptions options) const;

    /// The symbols a name query (SymbolQuery) matches, best
    /// first, at most `limit`: the search index's hits, the symbols merged
    /// since it was built and the open sessions' own symbols — scanned
    /// directly — under one ranking, each with its canonical site. A query
    /// naming files also finds the symbols only those files see — locals,
    /// parameters, internal-linkage entities — among what each declares.
    /// A symbol no source places is left out. Empty for a query by id or
    /// place.
    std::vector<Located> search(const SymbolQuery& query, std::size_t limit) const;

    /// A name unqualified completion may offer from the index, with the
    /// file of its canonical declaration: invalid for one of a preamble,
    /// which the file reaches already.
    struct Completable {
        SymbolRef symbol;
        Fid file;
    };

    /// The names `prefix` matches that unqualified completion in `file` may
    /// offer — names of namespaces and unscoped enumerators
    /// (SymbolFlags::Completable) linked across files — best first, at most
    /// `limit`: the project's, the open buffers' and those of `file`'s
    /// preamble. Whether the limit cut them short.
    struct Completables {
        std::vector<Completable> names;
        bool exhausted = true;
    };

    Completables completables(llvm::StringRef prefix, Fid file, std::size_t limit) const;

    /// The symbols a locator query names: by id; by place — the symbol
    /// under a cursor, or those defined on a line of the file's serving
    /// source; or by pattern, where every symbol spelling the name exactly
    /// (in any case) is the answer when there is one, and the ranked
    /// matches stand as candidates otherwise. A template's name without
    /// arguments names the template, not its specializations.
    std::vector<Located> locate(const SymbolQuery& query) const;

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
    /// is the first-hit order (session indexes, preamble indexes, overlays,
    /// disk); DiskFirst the collect-all order.
    enum class Order : std::uint8_t { LiveFirst, DiskFirst };

    struct SourceMask {
        bool shard = true;
        bool session = true;
        bool preamble = true;
        bool overlay = true;
    };

    using RelationVisitor = llvm::function_ref<bool(const IndexSource&, const Relation&)>;

    /// A search's ranked symbols before their sites are resolved, and
    /// whether the limit cut the ranking short.
    struct Ranked {
        NameRank rank;
        SymbolRef symbol;
        /// The file a file's own symbol was found in, which holds its
        /// entries; invalid for a project symbol.
        Fid anchor;
    };

    struct RankedHits {
        std::vector<Ranked> hits;
        bool exhausted = true;
    };

    RankedHits ranked_search(const SymbolQuery& query, std::size_t limit) const;

    std::optional<DeclaredText> declared_text(SymbolHash hash, Fid anchor, RelationKind kind) const;

    /// Relations of `kind` grouped by their target symbol, each with the
    /// sites spelling it, the targets resolved to their canonical sites.
    std::vector<Edge> edges(SymbolHash hash, Fid anchor, RelationKind kind) const;

    /// The sites of the symbol's relations of `kind`, added to `by_target`
    /// under their targets.
    void collect_by_target(SymbolHash hash,
                           Fid anchor,
                           RelationKind kind,
                           llvm::DenseMap<SymbolHash, std::vector<Site>>& by_target) const;

    /// The functions whose bodies reference `hash`, each with its
    /// references, weak ones included.
    std::vector<Edge> referencing_functions(SymbolHash hash, Fid anchor) const;

    /// The declarations of the functions of `hash`'s name and linkage in
    /// its scope that nothing defines, when `hash` is a function defined
    /// out of its class with no other declaration: the one it parted from
    /// when a parameter changed on one side, which wants the same edit.
    std::vector<Site> mismatched_declarations(SymbolHash hash, Fid anchor) const;

    /// The functions `source` defines.
    std::vector<DefinedFunction> defined_functions(const IndexSource& source) const;

    /// The edges of the sites grouped by neighbour, each neighbour at its
    /// canonical site.
    std::vector<Edge> located_edges(llvm::DenseMap<SymbolHash, std::vector<Site>>& by_target) const;

    /// A relation target, anchored at the file the relation was read from: a
    /// file naming a symbol below external scope holds one of its entries.
    struct Target {
        SymbolHash symbol;
        Fid anchor;
    };

    /// The distinct target symbols of the symbol's relations of `kind`
    /// (bases, derived types, overrides), in first-seen order.
    llvm::SmallVector<Target> targets(SymbolHash hash, Fid anchor, RelationKind kind) const;

    /// The same followed from each target on: every derived type, every
    /// override, every overridden method at any depth, each once.
    llvm::SmallVector<Target> transitive_targets(SymbolHash hash,
                                                 Fid anchor,
                                                 RelationKind kind) const;

    /// Whether some unit reported a definition of the symbol: an open
    /// session's table knows only its own unit, the project table all.
    bool reported_defined(SymbolHash hash, Fid anchor) const;

    /// A declaring relation's site and the whole declaration it names (the
    /// definition's body included); the site itself when the relation carries
    /// no extent.
    struct Placed {
        Site site;
        Site extent;
    };

    /// first_site with the declaration's extent.
    std::optional<Placed> first_placed(SymbolHash hash, Fid anchor, RelationKind kind) const;

    /// The symbol's canonical site: its definition, or a declaration when
    /// nothing defines it (pure virtuals, externs, decl-only APIs).
    std::optional<Placed> canonical_placed(SymbolHash hash, Fid anchor) const;

    /// The disk files holding the symbol's entries, by its scope (see the
    /// class comment on anchors).
    void each_disk_file(SymbolHash hash, Fid anchor, llvm::function_ref<void(Fid)> visit) const;

    /// The one federation walk every relation query is a fold over. The
    /// visitor returns false to stop.
    void for_each_relation(SymbolHash hash,
                           Fid anchor,
                           RelationKind kind,
                           Order order,
                           SourceMask mask,
                           RelationVisitor visitor) const;

    /// The `kind` relations of `hash` in the entries of a live TU index's
    /// other files than its focus file (an overlay's headers, a buffer's
    /// imported declarations) that may contribute results: files that are
    /// themselves open serve a buffer-true index through their sessions,
    /// and entries of text the disk no longer holds point nowhere (clause
    /// 2). False once the visitor stopped.
    bool visit_overlay_relations(const TUIndex& state,
                                 SymbolHash hash,
                                 RelationKind kind,
                                 RelationVisitor visitor) const;

    /// The file's instantiation shard when it resolves in the shard's text.
    const InstantiationShard* instantiations_of(Fid file, const Shard& shard) const;

    /// The text a source's offsets index: the buffer, or the file read from
    /// disk (kept alive in `storage`) and verified against the index's
    /// content hash.
    std::optional<llvm::StringRef> source_text(const IndexSource& source,
                                               std::unique_ptr<llvm::MemoryBuffer>& storage) const;

    const ProjectIndex& index;
    FileTable& files;
    const FreshnessGate* gate;
    const LiveSources* live;

    /// Whether answers take in what instantiations resolve (written_only).
    bool resolved = true;
};

}  // namespace clice::index

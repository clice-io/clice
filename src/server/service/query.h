#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "feature/feature.h"
#include "sched/workspace.h"
#include "semantic/symbol.h"
#include "server/protocol/position.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/MemoryBuffer.h"

namespace clice {

class IndexPump;
struct ASTProjectionTable;
struct RowSource;
struct Session;
struct SessionStore;

using index::SymbolRef;

/// Which of a file's index sources serves it right now — the freshness
/// contract's arbitration, decided in one place (IndexQuery::serving_source).
struct ServingSource {
    enum class By : std::uint8_t {
        /// Nothing may serve: no rows, or rows describing text the file no
        /// longer holds.
        None,
        /// The open session's own file index (clauses 1 and 3).
        SessionRows,
        /// The disk shard under closed-file rules (clauses 2 and 4).
        ShardAsClosed,
    };

    By by = By::None;
    const index::Shard* rows = nullptr;
    Coordinates coords;

    explicit operator bool() const {
        return by != By::None;
    }
};

/// The buffer-side sources a query reads besides disk truth. All null is
/// the disk-only view of the agent transport and headless tools: open
/// buffers never participate, every file answers as if closed, and no
/// "awaiting reindex" state exists.
struct QuerySources {
    const SessionStore* sessions = nullptr;
    const ASTProjectionTable* projections = nullptr;
    const IndexPump* pump = nullptr;
};

/// How an agent names a symbol, tried in this order: by handle; by name,
/// case-insensitively, optionally narrowed to a path — a bare file name
/// matches the site's file name, anything longer the tail of its path;
/// by path and 1-based line.
struct SymbolLocator {
    std::optional<index::SymbolHash> symbol;
    llvm::StringRef name;
    llvm::StringRef path;
    std::optional<int> line;
};

/// Read-only queries over every index source: disk shards, open sessions'
/// file indexes, PCH overlays and the buffers' own preamble rows. Holds no
/// index data of its own — Workspace owns the disk-derived index, the AST
/// family's projections own the per-buffer indexes.
///
/// Every answer is a domain value (Site, SymbolRef): protocol shapes are
/// the transports' business (server/protocol/lsp_projection.h). Every
/// method is synchronous and its results borrow the state they were read
/// from — consume them before the next suspension point.
///
/// Freshness contract — results may be incomplete, by design:
///
///   1. Cursor resolution (turning an offset in the request's file into a
///      symbol) is accurate: callers that hold an open session await its
///      compile first (Features awaits ensure_compiled with the same
///      no-timeout posture as every AST-backed request), so the session's
///      file index describes the buffer being pointed at. For closed files
///      the merged shard resolves against its own stored content snapshot
///      — unless the file's own content changed and its reindex is still
///      pending, in which case the cursor is unresolvable (clause 2).
///   2. Cross-file contributions honor the pump's pending state: a file
///      awaiting reindex only because a dependency changed keeps serving
///      its previous rows (its own text did not move), while a file whose
///      own content changed has its contribution skipped until the reindex
///      lands — stale rows would point at text that no longer exists.
///   3. Open sessions whose compile has not (re)finished are skipped
///      entirely: their buffer may have diverged from the last file index,
///      and unlike closed files their reindex is the next compile, which
///      the current file's request already awaits.
///   4. An open session without a current file index is served by the
///      file's shard under closed-file rules — but only while the buffer
///      is byte-identical to the content the rows were built from
///      (Shard::matches_content). This is what serves documents opened
///      for reading before any compile is invested in them; the moment
///      the buffer diverges (an edit, restored unsaved text) the shard
///      withdraws and clauses 1-3 govern again. Arbitration is strict:
///      a current file index always wins over the shard (never both).
///
///   Symbol identity lookups (symbol_info: hash → name/kind) are not
///   gated: a hash identifies one symbol, so even a stale shard answers
///   them correctly.
class IndexQuery {
public:
    IndexQuery(Workspace& workspace, QuerySources sources);

    /// The freshness contract's single arbitration: which source serves
    /// `file` right now, with the coordinates its rows are expressed in.
    /// An open session served by its shard reads in buffer coordinates —
    /// the bytes are identical, and the buffer's text is at hand.
    ServingSource serving_source(Fid file) const;

    /// The session's shard when its content is byte-identical to the
    /// buffer (clause 4's content gate alone, regardless of what serves
    /// the rows): disk-truth products such as manifest edges are only
    /// meaningful against text the index described.
    const index::Shard* matching_shard(const Session& session) const;

    /// The session's PCH envelope while the buffer still starts with the
    /// exact preamble the blob was built from; null when there is no PCH
    /// or a deferred rebuild left an old blob behind a moved preamble.
    std::shared_ptr<index::TUIndex> preamble_blob(const Session& session) const;

    /// The symbol whose occurrence covers `offset` in the file's serving
    /// source (clauses 1 and 4), and the site of that occurrence. A session
    /// served by its own rows also resolves through the preamble region
    /// of its PCH overlay — compiled into the PCH, invisible to the
    /// per-edit index, spelled in the same buffer coordinates.
    struct Cursor {
        index::SymbolHash symbol = 0;
        Site site;
    };

    std::optional<Cursor> symbol_at(Fid file, std::uint32_t offset) const;

    /// A symbol's name and kind, from whichever table knows the hash: open
    /// sessions, the project index, PCH overlays, then the per-file shards
    /// (TU-local names live only there).
    std::optional<SymbolRef> symbol_info(index::SymbolHash hash) const;

    /// Every site carrying a relation of `kind` for the symbol, across all
    /// serving sources, deduplicated — a row present in both a disk shard
    /// and an overlay comes out identical.
    std::vector<Site> sites(index::SymbolHash hash, RelationKind kind) const;

    /// The first site carrying the relation, live sources first: an open
    /// buffer's rows, its preamble region, PCH overlays (the definition as
    /// seen under the live context — present even when no disk TU was
    /// indexed), then disk shards.
    std::optional<Site> first_site(index::SymbolHash hash, RelationKind kind) const;

    /// The symbol's canonical site: its definition, or a declaration when
    /// nothing defines it (pure virtuals, externs, decl-only APIs).
    std::optional<Site> canonical_site(index::SymbolHash hash) const;

    /// Relations of `kind` grouped by their target symbol, each with the
    /// sites spelling it: the shape behind call hierarchies.
    struct Group {
        index::SymbolHash symbol = 0;
        std::vector<Site> sites;
    };

    std::vector<Group> grouped(index::SymbolHash hash, RelationKind kind) const;

    /// The distinct target symbols of the symbol's relations of `kind`
    /// (bases, derived types, overrides), in first-seen order.
    llvm::SmallVector<index::SymbolHash> targets(index::SymbolHash hash, RelationKind kind) const;

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
    std::vector<Site> target_sites(index::SymbolHash hash, RelationKind kind) const;

    /// Sites implementing the symbol: derived types for a class-like
    /// symbol, override targets otherwise.
    std::vector<Site> implementation(index::SymbolHash hash) const;

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

    std::optional<Definition> definition_text(index::SymbolHash hash) const;

    /// The source line a site lies on, for previews; empty when the text
    /// is unavailable (see definition_text on the disk re-read).
    std::string context_line(const Site& site) const;

    /// A symbol together with its canonical site.
    struct Located {
        SymbolRef symbol;
        Site site;
    };

    std::optional<Located> resolve(index::SymbolHash hash) const;

    /// Symbols whose name contains `query` (case-insensitive), best
    /// matches first — exact name, then prefix, then substring, ties by
    /// name — cut to `limit` after ranking. Only symbols with a definition
    /// site are listed.
    std::vector<Located> search(llvm::StringRef query, std::size_t limit) const;

    /// The symbols a locator names; several when a name is ambiguous.
    std::vector<Located> locate(const SymbolLocator& locator) const;

    /// Every project symbol with a definition site in the file's serving
    /// source, anchored at the definition's name token.
    std::vector<Located> definitions_in(Fid file) const;

    /// The include edges of the session's document, the input of the
    /// document-link projection: from its own TU manifest when it has one,
    /// else from the contributing TUs' manifests (a header reached only
    /// through source TUs — its directives are nodes hanging off the
    /// header's own node there). Only manifests that entered the document
    /// at the serving shard's content generation contribute; empty when
    /// the file was never indexed.
    std::vector<feature::IndexIncludeEdge> include_edges(const Session& session) const;

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

    using RelationVisitor = llvm::function_ref<bool(const RowSource&, const index::Relation&)>;

    /// The one federation walk every relation query is a fold over. The
    /// visitor returns false to stop.
    void for_each_relation(index::SymbolHash hash,
                           RelationKind kind,
                           Order order,
                           SourceMask mask,
                           RelationVisitor visitor) const;

    /// Open sessions whose file index is current (clause 3).
    void visit_sessions(llvm::function_ref<bool(Fid, const Session&)> visitor) const;

    /// Each distinct PCH overlay blob once (sessions sharing a preamble
    /// share one blob). Overlays are the only index source for headers as
    /// seen under a live buffer's context; their header entries hold
    /// disk-derived coordinates that buffer edits cannot move, so no
    /// session gating applies — the blob's own staleness follows the
    /// PCH's dependency discipline.
    void visit_overlays(llvm::function_ref<bool(const index::TUIndex&)> visitor) const;

    /// Each open session whose overlay preamble entry may serve: the blob
    /// was built from this very file (identical preambles share one PCH,
    /// but macro USRs embed the source path) and the buffer still starts
    /// with the blob's preamble.
    void visit_preambles(
        llvm::function_ref<bool(Fid, const Session&, const index::TUIndex&)> visitor) const;

    /// The header entries of an overlay that may contribute results:
    /// files that are themselves open serve buffer-true rows through their
    /// sessions, files whose disk content changed await their reindex
    /// (clause 2), and synthesized context artifacts must never send the
    /// user into the cache.
    void visit_overlay_files(const index::TUIndex& state,
                             llvm::function_ref<bool(const RowSource&)> visitor) const;

    /// The session's PCH overlay blob, or null when it has no PCH or the
    /// blob is unreadable.
    std::shared_ptr<index::TUIndex> overlay_of(const Session& session) const;

    /// A current session's own rows, and its overlay's preamble rows, as
    /// row sources in buffer coordinates.
    RowSource session_source(Fid path_id, const Session& session) const;
    RowSource preamble_source(Fid path_id,
                              const Session& session,
                              const index::TUIndex& state) const;

    /// Clause 2: a closed file whose own content changed and whose reindex
    /// has not landed contributes nothing.
    bool skip_stale_contribution(Fid file) const;

    bool is_open(Fid file) const;

    /// The text a source's offsets index: the buffer, the blob's stored
    /// text, or the disk re-read for pure-ASCII blobs (kept alive in
    /// `storage`), verified against the rows' content hash.
    std::optional<llvm::StringRef> source_text(const RowSource& source,
                                               std::unique_ptr<llvm::MemoryBuffer>& storage) const;

    Workspace& workspace;
    QuerySources sources;
};

}  // namespace clice

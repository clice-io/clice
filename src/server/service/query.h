#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "semantic/symbol.h"
#include "server/protocol/agentic.h"
#include "server/state/workspace.h"

#include "kota/ipc/lsp/position.h"
#include "kota/ipc/lsp/protocol.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"

namespace clice {

namespace protocol = kota::ipc::protocol;
namespace lsp = kota::ipc::lsp;

class Indexer;
struct Session;
struct SessionStore;

/// Information about a symbol at a given position.
struct SymbolInfo {
    index::SymbolHash hash = 0;
    std::string name;
    SymbolKind kind;
    std::string uri;
    protocol::Range range;
};

/// A symbol resolved from an agentic locator (symbol id, name, or path+line).
struct ResolvedSymbol {
    index::SymbolHash hash = 0;
    std::string name;
    SymbolKind kind;
    std::string file;
    int line = 0;
};

/// Read-only index query layer.
///
/// IndexQuery holds no index data of its own.  All persistent data lives in
/// Workspace (disk-derived ProjectIndex + Shard blobs) and per-file
/// data lives in Session (file index from unsaved buffers).
///
/// Responsibilities:
///   - Cross-file navigation queries (definition, references, hierarchy)
///   - Symbol search (workspace/symbol)
///
/// NOT responsible for:
///   - Compilation — handled by Compiler
///   - Background indexing — handled by Indexer
///   - Document lifecycle — handled by MasterServer
///
/// Freshness contract — results may be incomplete, by design:
///
///   1. Cursor resolution (turning an offset in the request's file into a
///      symbol) is accurate: callers that hold an open session await its
///      compile first (FeatureRouter awaits ensure_compiled with the same
///      no-timeout posture as every AST-backed request), so the session's
///      file index describes the buffer being pointed at. For closed files
///      the merged shard resolves against its own stored content snapshot
///      — unless the file's own content changed and its reindex is still
///      pending, in which case the cursor is unresolvable (clause 2).
///   2. Cross-file contributions honor the indexer's pending state: a file
///      awaiting reindex only because a dependency changed keeps serving
///      its previous rows (its own text did not move), while a file whose
///      own content changed has its contribution skipped until the reindex
///      lands — stale rows would point at text that no longer exists.
///   3. Open sessions whose compile has not (re)finished are skipped
///      entirely (see visit_sessions): their buffer may have diverged from
///      the last file index, and unlike closed files their reindex is the
///      next compile, which the current file's request already awaits.
///
///   Symbol identity lookups (find_symbol_info: hash → name/kind) are not
///   gated: a hash identifies one symbol, so even a stale shard answers
///   them correctly.
///
///   TODO: a blocking query mode (await the pending reindexes instead of
///   skipping) for consumers that need completeness over latency. Not
///   implemented — no current caller wants to stall on a full queue.
///   TODO: a dedicated "is the index ready?" request so agent consumers
///   can distinguish "no references" from "not indexed yet". Not
///   implemented — needs protocol design.

/// Which index sources an IndexQuery instance serves from.
struct IndexQueryOptions {
    /// Disk truth only: buffer state — open sessions' file indexes and
    /// their PCH overlays — never participates, and open files answer
    /// from their shards exactly like closed ones. This is the agentic
    /// transport's mode: agents read files from disk, so positions from
    /// unsaved buffers would not match what they read.
    bool disk_only = false;
};

class IndexQuery {
public:
    /// Visitor for iterating open Sessions.  Returns false to stop early.
    using SessionVisitor =
        std::function<bool(std::uint32_t server_path_id, const Session& session)>;

    IndexQuery(Workspace& workspace,
               const SessionStore& sessions,
               const Indexer& indexer,
               IndexQueryOptions options = {}) :
        workspace(workspace), sessions(sessions), indexer(indexer), options(options) {}

    /// Query relations (Definition, Reference, etc.) for a symbol at cursor.
    /// @param session  Active Session for this file, or nullptr to use the disk shards only.
    std::vector<protocol::Location> query_relations(llvm::StringRef path,
                                                    const protocol::Position& position,
                                                    RelationKind kind,
                                                    Session* session);

    /// Query definition locations for the symbol at cursor. Standing on the
    /// definition itself navigates to the declarations (and any sibling
    /// definitions) instead, so definition and declaration sites alternate;
    /// an inline-defined symbol with no separate declaration answers with
    /// the definition site itself. A symbol with no definition at all (pure
    /// virtual, extern, pre-C++17 class constant) answers with its
    /// declarations rather than an empty result.
    /// @param session  Active Session for this file, or nullptr to use the disk shards only.
    std::vector<protocol::Location> query_definition(llvm::StringRef path,
                                                     const protocol::Position& position,
                                                     Session* session);

    /// Query declaration locations for the symbol at cursor: declarations
    /// plus the definition (symbols defined inline have no separate
    /// Declaration relation), minus the site the cursor stands on — the
    /// mirror half of query_definition's alternation.
    /// @param session  Active Session for this file, or nullptr to use the disk shards only.
    std::vector<protocol::Location> query_declaration(llvm::StringRef path,
                                                      const protocol::Position& position,
                                                      Session* session);

    /// Query between-symbol relations (TypeDefinition, ...) for the symbol
    /// at cursor and resolve each target symbol to its canonical location —
    /// the two-hop query behind go-to-type-definition.
    /// @param session  Active Session for this file, or nullptr.
    std::vector<protocol::Location> query_symbol_targets(llvm::StringRef path,
                                                         const protocol::Position& position,
                                                         RelationKind kind,
                                                         Session* session);

    /// Locations implementing the symbol at cursor: derived types for a
    /// class-like symbol, Implementation relation targets (overrides)
    /// otherwise.
    /// @param session  Active Session for this file, or nullptr.
    std::vector<protocol::Location> query_implementation(llvm::StringRef path,
                                                         const protocol::Position& position,
                                                         Session* session);

    /// Look up symbol info (hash, name, kind, range) at a cursor position.
    /// @param session  Active Session for this file, or nullptr.
    std::optional<SymbolInfo> lookup_symbol(const std::string& uri,
                                            llvm::StringRef path,
                                            const protocol::Position& position,
                                            Session* session);

    /// Find the definition location of a symbol by hash.
    std::optional<protocol::Location> find_definition_location(index::SymbolHash hash);

    /// The symbol's canonical location: its definition, or its declaration
    /// when nothing defines it (pure virtuals, externs, decl-only APIs).
    std::optional<protocol::Location> find_symbol_location(index::SymbolHash hash);

    /// Resolve a symbol hash into a SymbolInfo at its canonical location.
    std::optional<SymbolInfo> resolve_symbol(index::SymbolHash hash);

    /// Find a symbol's name and kind by hash.
    bool find_symbol_info(index::SymbolHash hash, std::string& name, SymbolKind& kind) const;

    /// Resolve a hierarchy item (from stored data or by position lookup).
    /// @param session  Active Session for this file, or nullptr.
    std::optional<SymbolInfo> resolve_hierarchy_item(const std::string& uri,
                                                     llvm::StringRef path,
                                                     const protocol::Range& range,
                                                     const std::optional<protocol::LSPAny>& data,
                                                     Session* session);

    /// Find incoming calls to a function.
    std::vector<protocol::CallHierarchyIncomingCall> find_incoming_calls(index::SymbolHash hash);

    /// Find outgoing calls from a function.
    std::vector<protocol::CallHierarchyOutgoingCall> find_outgoing_calls(index::SymbolHash hash);

    /// Find supertypes (base classes) of a type.
    std::vector<protocol::TypeHierarchyItem> find_supertypes(index::SymbolHash hash);

    /// Find subtypes (derived classes) of a type.
    std::vector<protocol::TypeHierarchyItem> find_subtypes(index::SymbolHash hash);

    /// Search symbols by name substring.
    std::vector<protocol::SymbolInformation> search_symbols(llvm::StringRef query,
                                                            std::size_t max_results = 100);

    /// The three queries below serve the agentic tools and are only ever
    /// called on the disk_only instance: their bodies read shards alone,
    /// with no session or overlay passes to disable.

    struct DefinitionText {
        std::string file;
        int start_line;
        int end_line;
        std::string text;
    };

    /// Get full definition text for a symbol, using stored index ranges and content.
    std::optional<DefinitionText> get_definition_text(index::SymbolHash hash);

    struct ReferenceWithContext {
        std::string file;
        int line;
        std::string context;
    };

    /// Collect references (or definitions) with context lines from stored content.
    std::vector<ReferenceWithContext> collect_references(index::SymbolHash hash, RelationKind kind);

    /// Resolve an agentic locator to the set of matching symbols, by symbol
    /// id, by name, or by path+line — the three strategies the agentic
    /// read/definition/references tools share.
    std::vector<ResolvedSymbol> locate_symbols(const agentic::ReadSymbolParams& loc);

    /// Whether a file's shard sits this query out: its own disk content
    /// changed and awaits reindexing (clause 2), or — unless disk_only —
    /// the file is open and its session serves it instead.
    bool skip_shard(std::uint32_t path_id) const;

    /// Iterate all open Sessions with valid, up-to-date file indices.
    void visit_sessions(SessionVisitor visitor) const;

    /// Convert internal SymbolKind to LSP SymbolKind.
    static protocol::SymbolKind to_lsp_symbol_kind(SymbolKind kind);

    /// Build hierarchy items from SymbolInfo.
    static protocol::CallHierarchyItem build_call_hierarchy_item(const SymbolInfo& info);
    static protocol::TypeHierarchyItem build_type_hierarchy_item(const SymbolInfo& info);

private:
    /// Result of resolving a symbol at a cursor position.
    struct CursorHit {
        index::SymbolHash hash = 0;
        protocol::Range range{};
    };

    /// Resolve the symbol at (position), checking Session's file_index first
    /// then falling back to Workspace's disk shards.
    CursorHit resolve_cursor(llvm::StringRef path,
                             const protocol::Position& position,
                             Session* session);

    /// Assemble the locations of a symbol's relation rows of one kind
    /// across all index sources, deduplicated and sorted.
    std::vector<protocol::Location> collect_relation_locations(index::SymbolHash hash,
                                                               RelationKind kind);

    /// The request file's URI in the pool's canonical spelling — the form
    /// collected locations carry. Empty when the path is unknown to the
    /// pool (no location can name it then either).
    std::string self_uri(llvm::StringRef path);

    /// Visit each distinct PCH overlay blob once (sessions sharing a
    /// preamble share one blob). Overlays are the only index source for
    /// headers as seen under a live buffer's context (novel unsaved
    /// preamble edits, headers not reachable from any indexed disk TU);
    /// their header entries hold disk-derived coordinates that buffer
    /// edits cannot move, so no session gating applies — the blob's own
    /// staleness follows the PCH's dependency discipline. Identical rows
    /// also present in disk shards are collapsed by per-location dedup at
    /// result assembly. Return false from the visitor to stop.
    void visit_overlays(llvm::function_ref<bool(const index::TUIndex&)> visitor) const;

    /// Visit each open session whose overlay preamble entry may serve
    /// (see serves_preamble), paired with that blob.
    void visit_preambles(llvm::function_ref<bool(std::uint32_t server_path_id,
                                                 const Session& session,
                                                 const index::TUIndex& state)> visitor) const;

    /// The PCH overlay of a session, or nullptr when it has no PCH or the
    /// blob is unreadable.
    std::shared_ptr<index::TUIndex> overlay_of(const Session& session) const;

    /// Whether a session's overlay preamble entry may serve: the blob was
    /// built from this very file (identical preambles share one PCH, but
    /// macro USRs embed the source path) and the buffer still starts with
    /// the exact preamble the blob was built from (compared by hash — the
    /// text itself is not stored).
    bool serves_preamble(const Session& session, const index::TUIndex& state) const;

    /// Whether an overlay file entry may contribute results. Filters
    /// synthesized context artifacts (their positions live in
    /// cache-directory files the user should never be sent to), files
    /// that are themselves open — their sessions serve buffer-true rows,
    /// while overlay rows describe the disk snapshot and would map onto
    /// the edited buffer at the wrong lines — and files whose own disk
    /// content changed and awaits reindexing (freshness contract, clause
    /// 2, same as shard contributions).
    bool should_serve_overlay_file(llvm::StringRef path) const;

    /// Collect relations grouped by target symbol, across all index sources.
    void collect_grouped_relations(
        index::SymbolHash hash,
        RelationKind kind,
        llvm::DenseMap<index::SymbolHash, std::vector<protocol::Range>>& target_ranges);

    /// Collect unique target symbol hashes for a relation kind.
    void collect_unique_targets(index::SymbolHash hash,
                                RelationKind kind,
                                llvm::SmallVectorImpl<index::SymbolHash>& targets);

    /// One location per unique relation target, each resolved to its
    /// canonical site.
    std::vector<protocol::Location> resolve_target_locations(index::SymbolHash hash,
                                                             RelationKind kind);

    /// Find one location carrying the given relation kind for the symbol.
    std::optional<protocol::Location> find_relation_location(index::SymbolHash hash,
                                                             RelationKind kind);

    /// Check whether a path_id has an active Session.
    bool is_path_open(std::uint32_t path_id) const;

    /// Freshness contract, clause 2: whether a closed file's contribution
    /// must be skipped because its own content changed and the reindex has
    /// not landed yet. O(1) per candidate file, no I/O.
    bool skip_stale_contribution(std::uint32_t path_id) const;

    Workspace& workspace;
    const SessionStore& sessions;
    const Indexer& indexer;
    IndexQueryOptions options;
};

}  // namespace clice

#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "semantic/relation_kind.h"
#include "semantic/symbol_kind.h"
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
/// Workspace (disk-derived ProjectIndex + MergedIndex shards) and per-file
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
    /// @param session  Active Session for this file, or nullptr to use MergedIndex only.
    std::vector<protocol::Location> query_relations(llvm::StringRef path,
                                                    const protocol::Position& position,
                                                    RelationKind kind,
                                                    Session* session);

    /// Query between-symbol relations (Implementation, TypeDefinition, ...)
    /// for the symbol at cursor and resolve each target symbol to its
    /// definition location — the two-hop query behind go-to-implementation
    /// and go-to-type-definition.
    /// @param session  Active Session for this file, or nullptr.
    std::vector<protocol::Location> query_symbol_targets(llvm::StringRef path,
                                                         const protocol::Position& position,
                                                         RelationKind kind,
                                                         Session* session);

    /// Look up symbol info (hash, name, kind, range) at a cursor position.
    /// @param session  Active Session for this file, or nullptr.
    std::optional<SymbolInfo> lookup_symbol(const std::string& uri,
                                            llvm::StringRef path,
                                            const protocol::Position& position,
                                            Session* session);

    /// Find the definition location of a symbol by hash.
    std::optional<protocol::Location> find_definition_location(index::SymbolHash hash);

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
    /// then falling back to Workspace's MergedIndex.
    CursorHit resolve_cursor(llvm::StringRef path,
                             const protocol::Position& position,
                             Session* session);

    /// Visit each distinct PCH overlay blob once (sessions sharing a
    /// preamble share one blob). Overlays are the only index source for
    /// headers as seen under a live buffer's context (novel unsaved
    /// preamble edits, headers not reachable from any indexed disk TU);
    /// their header entries hold disk-derived coordinates that buffer
    /// edits cannot move, so no session gating applies — the blob's own
    /// staleness follows the PCH's dependency discipline. Identical rows
    /// also present in disk shards are collapsed by per-location dedup at
    /// result assembly. Return false from the visitor to stop.
    void visit_overlays(llvm::function_ref<bool(const index::PreambleState&)> visitor) const;

    /// Visit each open session whose overlay preamble entry may serve
    /// (see serves_preamble), paired with that blob.
    void visit_preambles(llvm::function_ref<bool(std::uint32_t server_path_id,
                                                 const Session& session,
                                                 const index::PreambleState& state)> visitor) const;

    /// The PCH overlay of a session, or nullptr when it has no PCH or the
    /// blob is unreadable.
    std::shared_ptr<index::PreambleState> overlay_of(const Session& session) const;

    /// Whether a session's overlay preamble entry may serve: the blob was
    /// built from this very file (identical preambles share one PCH, but
    /// macro USRs embed the source path) and the buffer still starts with
    /// the blob's stored preamble text.
    bool serves_preamble(const Session& session, const index::PreambleState& state) const;

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

    /// Resolve a symbol hash into a SymbolInfo with definition location.
    std::optional<SymbolInfo> resolve_symbol(index::SymbolHash hash);

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

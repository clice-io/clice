#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "eventide/ipc/lsp/position.h"
#include "eventide/ipc/lsp/protocol.h"
#include "semantic/relation_kind.h"
#include "semantic/symbol_kind.h"
#include "server/workspace.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"

namespace clice {

namespace et = eventide;
namespace protocol = et::ipc::protocol;
namespace lsp = et::ipc::lsp;

struct Session;

/// Information about a symbol at a given position.
struct SymbolInfo {
    index::SymbolHash hash = 0;
    std::string name;
    SymbolKind kind;
    std::string uri;
    protocol::Range range;
};

/// Provides query methods for cross-file navigation over Workspace index data
/// and open Session file indices.
///
/// Indexer does NOT own any index state.  All persistent data lives in
/// Workspace (disk-derived ProjectIndex + MergedIndex shards) and per-file
/// data lives in Session (OpenFileIndex from unsaved buffers).
///
/// Background indexing scheduling is driven by MasterServer; Indexer is the
/// pure query layer.
class Indexer {
public:
    /// @param workspace  Project-wide persistent state (ProjectIndex, MergedIndex shards).
    /// @param sessions   Map of open editing sessions, keyed by server-level path_id.
    /// @param is_file_open  Callback that returns true if a *project-level* path_id
    ///        has an active Session.  Used to skip stale MergedIndex shards when
    ///        fresher OpenFileIndex data is available.
    Indexer(Workspace& workspace,
            llvm::DenseMap<std::uint32_t, Session>& sessions,
            std::function<bool(std::uint32_t)> is_file_open = {}) :
        workspace(workspace), sessions(sessions), is_file_open(std::move(is_file_open)) {}

    /// Merge a TUIndex result into Workspace's ProjectIndex and MergedIndex shards.
    void merge(const void* tu_index_data, std::size_t size);

    /// Save Workspace's ProjectIndex and MergedIndex shards to disk.
    void save(llvm::StringRef index_dir);

    /// Load Workspace's ProjectIndex and MergedIndex shards from disk.
    void load(llvm::StringRef index_dir);

    /// Check whether a file needs re-indexing (stale or missing shard).
    bool need_update(llvm::StringRef file_path);

    /// Query relations (Definition, Reference, etc.) for a symbol at cursor.
    /// @param session  Active Session for this file, or nullptr to use MergedIndex only.
    std::vector<protocol::Location> query_relations(llvm::StringRef path,
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

    /// Check whether a project-level path_id has an active Session.
    bool is_proj_path_open(std::uint32_t proj_path_id) const {
        return is_file_open && is_file_open(proj_path_id);
    }

private:
    Workspace& workspace;
    llvm::DenseMap<std::uint32_t, Session>& sessions;

    /// Callback that checks if a *project-level* path_id has an active
    /// Session.  Set by the owner (e.g. MasterServer) to bridge the
    /// server-path-id-keyed sessions map to project-level path_ids.
    std::function<bool(std::uint32_t)> is_file_open;
};

}  // namespace clice

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "eventide/async/async.h"
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
class Compiler;
class WorkerPool;

/// Information about a symbol at a given position.
struct SymbolInfo {
    index::SymbolHash hash = 0;
    std::string name;
    SymbolKind kind;
    std::string uri;
    protocol::Range range;
};

/// Index query layer and background indexing scheduler.
///
/// Indexer holds no index data of its own.  All persistent data lives in
/// Workspace (disk-derived ProjectIndex + MergedIndex shards) and per-file
/// data lives in Session (OpenFileIndex from unsaved buffers).
///
/// Responsibilities:
///   - Cross-file navigation queries (definition, references, hierarchy)
///   - Symbol search (workspace/symbol)
///   - Background indexing scheduling (enqueue → idle timer → worker dispatch)
///   - Merging TUIndex results into Workspace's ProjectIndex
///
/// NOT responsible for:
///   - Compilation — handled by Compiler
///   - Document lifecycle — handled by MasterServer
class Indexer {
public:
    Indexer(et::event_loop& loop,
            Workspace& workspace,
            llvm::DenseMap<std::uint32_t, Session>& sessions,
            WorkerPool& pool,
            Compiler& compiler,
            std::function<bool(std::uint32_t)> is_file_open = {}) :
        loop(loop), workspace(workspace), sessions(sessions), pool(pool), compiler(compiler),
        is_file_open(std::move(is_file_open)) {}

    /// Add a file to the background indexing queue.
    void enqueue(std::uint32_t server_path_id);

    /// Schedule background indexing (respects idle timeout and dedup).
    void schedule();

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
    et::event_loop& loop;
    Workspace& workspace;
    llvm::DenseMap<std::uint32_t, Session>& sessions;
    WorkerPool& pool;
    Compiler& compiler;

    /// Callback that checks if a *project-level* path_id has an active
    /// Session.  Set by the owner (e.g. MasterServer) to bridge the
    /// server-path-id-keyed sessions map to project-level path_ids.
    std::function<bool(std::uint32_t)> is_file_open;

    /// Background indexing queue and scheduling state.
    std::vector<std::uint32_t> index_queue;
    std::size_t index_queue_pos = 0;
    bool indexing_active = false;
    bool indexing_scheduled = false;
    std::shared_ptr<et::timer> index_idle_timer;

    et::task<> run_background_indexing();
};

}  // namespace clice

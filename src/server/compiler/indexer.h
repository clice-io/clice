#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "semantic/relation_kind.h"
#include "semantic/symbol_kind.h"
#include "server/workspace/workspace.h"

#include "kota/async/async.h"
#include "kota/ipc/codec/json.h"
#include "kota/ipc/lsp/position.h"
#include "kota/ipc/lsp/progress.h"
#include "kota/ipc/lsp/protocol.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"

namespace clice {

namespace protocol = kota::ipc::protocol;
namespace lsp = kota::ipc::lsp;

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
/// data lives in Session (file index from unsaved buffers).
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
    /// Visitor for iterating open Sessions.  Returns false to stop early.
    using SessionVisitor =
        std::function<bool(std::uint32_t server_path_id, const Session& session)>;

    Indexer(kota::event_loop& loop,
            Workspace& workspace,
            WorkerPool& pool,
            Compiler& compiler,
            std::function<bool(std::uint32_t)> is_open = {},
            std::function<void(SessionVisitor)> each_session = {}) :
        loop(loop), bg_tasks(loop), workspace(workspace), pool(pool), compiler(compiler),
        is_open(std::move(is_open)), each_session(std::move(each_session)) {}

    /// Set the LSP peer for progress reporting.  Must be called before
    /// schedule() if progress notifications are desired.
    void set_peer(kota::ipc::JsonPeer* p) {
        peer = p;
    }

    /// Temporarily pause background indexing to give priority to user
    /// requests.  Indexing tasks already dispatched to workers continue,
    /// but no new tasks will be sent until resume_indexing() is called.
    void pause_indexing();

    /// Resume background indexing after a pause.
    void resume_indexing();

    /// RAII guard that pauses indexing for its lifetime.
    struct [[nodiscard]] ScopedPause {
        Indexer& indexer;

        explicit ScopedPause(Indexer& idx) : indexer(idx) {
            indexer.pause_indexing();
        }

        ~ScopedPause() {
            indexer.resume_indexing();
        }

        ScopedPause(const ScopedPause&) = delete;
        ScopedPause& operator=(const ScopedPause&) = delete;
    };

    ScopedPause scoped_pause() {
        return ScopedPause{*this};
    }

    /// Add a file to the background indexing queue.
    void enqueue(std::uint32_t server_path_id);

    /// Schedule background indexing (respects idle timeout and dedup).
    void schedule();

    /// Merge a TUIndex result into Workspace's ProjectIndex and MergedIndex shards.
    void merge(const void* tu_index_data, std::size_t size);

    /// Save Workspace's ProjectIndex and MergedIndex shards to the cache
    /// store ("index" namespace, Persistent policy).  Serialization runs
    /// on the event loop; each blob's commit (fsync + rename) is offloaded
    /// to the kota thread pool.
    kota::task<> save();

    /// Load Workspace's ProjectIndex and MergedIndex shards from the cache
    /// store, sweeping orphaned shard blobs.
    void load();

    /// Check whether a file needs re-indexing (stale or missing shard).
    bool need_update(llvm::StringRef file_path);

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

    /// Cancel background indexing and wait for all tasks to settle.
    kota::task<> stop();

    /// Iterate all open Sessions via the callback set at construction.
    void foreach_session(SessionVisitor visitor) const {
        if(each_session)
            each_session(std::move(visitor));
    }

    /// Invoke a callback with the Session for a specific server-level path_id.
    /// The callback is not invoked if no Session exists for that path_id.
    template <typename Fn>
    void with_session(std::uint32_t server_path_id, Fn&& fn) const {
        foreach_session([&](std::uint32_t id, const Session& session) -> bool {
            if(id == server_path_id) {
                fn(session);
                return false;
            }
            return true;
        });
    }

    /// Whether background indexing is currently idle (no active or queued work).
    bool is_idle() const {
        return !indexing_active && index_queue_pos >= index_queue.size();
    }

    /// Number of files remaining in the indexing queue.
    std::size_t pending_files() const {
        return index_queue_pos < index_queue.size() ? index_queue.size() - index_queue_pos : 0;
    }

    /// Total files that were enqueued in the current (or last) indexing round.
    std::size_t total_queued() const {
        return index_queue.size();
    }

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
        if(!is_open)
            return false;
        auto path = workspace.project_index.path_pool.path(proj_path_id);
        auto it = workspace.path_pool.cache.find(path);
        if(it == workspace.path_pool.cache.end())
            return false;
        return is_open(it->second);
    }

private:
    kota::event_loop& loop;
    kota::task_group<> bg_tasks;
    Workspace& workspace;
    WorkerPool& pool;
    Compiler& compiler;

    /// Checks if a server-level path_id has an open Session.
    std::function<bool(std::uint32_t)> is_open;

    /// Iterates all open Sessions with valid file indices.
    std::function<void(SessionVisitor)> each_session;

    /// LSP peer for progress reporting (optional, not owned).
    kota::ipc::JsonPeer* peer = nullptr;

    /// Background indexing queue and scheduling state.  pending_ids mirrors
    /// the un-consumed tail of index_queue so enqueue can dedupe; the queue
    /// is compacted once a round has fully drained.
    std::vector<std::uint32_t> index_queue;
    llvm::DenseSet<std::uint32_t> pending_ids;
    std::size_t index_queue_pos = 0;
    bool indexing_active = false;
    bool indexing_scheduled = false;
    std::shared_ptr<kota::timer> index_idle_timer;

    /// Pause/resume: when paused, new index tasks wait on this event.
    /// Uses a counter so nested pause/resume pairs work correctly.
    std::size_t pause_depth = 0;
    kota::event resume_event{true};

    kota::task<> run_background_indexing();
    kota::task<> index_one(std::uint32_t server_path_id, std::size_t index, std::size_t total);
};

}  // namespace clice

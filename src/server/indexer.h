#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "eventide/ipc/lsp/position.h"
#include "eventide/ipc/lsp/protocol.h"
#include "index/merged_index.h"
#include "index/project_index.h"
#include "semantic/relation_kind.h"
#include "semantic/symbol_kind.h"
#include "support/path_pool.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"

namespace clice {

namespace et = eventide;
namespace protocol = et::ipc::protocol;
namespace lsp = et::ipc::lsp;

/// In-memory index for an open file.  Kept separate from MergedIndex because
/// open files change frequently, are based on unsaved buffer content, and only
/// need to track the main file (headers are covered by PCH/PCM indexing).
struct OpenFileIndex {
    index::FileIndex file_index;
    index::SymbolTable symbols;
    std::string content;  ///< Buffer text at index time (for position mapping).

    /// Cached PositionMapper built from `content`.  Avoids re-scanning line
    /// offsets on every query.  Initialized by Indexer::set_open_file().
    std::optional<lsp::PositionMapper> mapper;

    /// Find the tightest occurrence containing `offset`.
    /// Returns (symbol_hash, LSP range) with positions already converted.
    std::optional<std::pair<index::SymbolHash, protocol::Range>>
        find_occurrence(std::uint32_t offset) const;

    /// Iterate relations matching `kind`, calling back with pre-converted ranges.
    /// Callback: (const index::Relation&, protocol::Range) -> bool (true = continue).
    template <typename Fn>
    void find_relations(index::SymbolHash hash, RelationKind kind, Fn&& fn) const {
        if(!mapper)
            return;
        auto it = file_index.relations.find(hash);
        if(it == file_index.relations.end())
            return;
        for(auto& r: it->second) {
            if(r.kind & kind) {
                auto start = mapper->to_position(r.range.begin);
                auto end = mapper->to_position(r.range.end);
                if(start && end) {
                    if(!fn(r, protocol::Range{*start, *end}))
                        return;
                }
            }
        }
    }
};

/// Wraps index::MergedIndex with a lazily-cached PositionMapper.
struct MergedIndexShard {
    index::MergedIndex index;
    mutable std::optional<lsp::PositionMapper> cached_mapper;

    /// Get or lazily build a PositionMapper from the index's stored content.
    const lsp::PositionMapper* mapper() const {
        if(!cached_mapper) {
            auto c = index.content();
            if(!c.empty()) {
                cached_mapper.emplace(c, lsp::PositionEncoding::UTF16);
            }
        }
        return cached_mapper ? &*cached_mapper : nullptr;
    }

    /// Invalidate the cached mapper (call after merge changes content).
    void invalidate_mapper() {
        cached_mapper.reset();
    }

    /// Find occurrence at byte offset.
    /// Returns (symbol_hash, LSP range) with positions already converted.
    std::optional<std::pair<index::SymbolHash, protocol::Range>>
        find_occurrence(std::uint32_t offset) const;

    /// Iterate relations matching `kind`, calling back with pre-converted ranges.
    /// Callback: (const index::Relation&, protocol::Range) -> bool (true = continue).
    template <typename Fn>
    void find_relations(index::SymbolHash hash, RelationKind kind, Fn&& fn) const {
        auto* m = mapper();
        if(!m)
            return;
        index.lookup(hash, kind, [&](const index::Relation& r) {
            auto start = m->to_position(r.range.begin);
            auto end = m->to_position(r.range.end);
            if(start && end) {
                return fn(r, protocol::Range{*start, *end});
            }
            return true;
        });
    }
};

/// Information about a symbol at a given position.
struct SymbolInfo {
    index::SymbolHash hash = 0;
    std::string name;
    SymbolKind kind;
    std::string uri;
    protocol::Range range;
};

/// Owns all index state (ProjectIndex, MergedIndex shards, open file indices)
/// and provides query methods for cross-file navigation.
///
/// Background indexing scheduling is driven by MasterServer; Indexer is the
/// pure data + query layer.
class Indexer {
public:
    explicit Indexer(PathPool& path_pool) : path_pool(path_pool) {}

    /// Merge a TUIndex result into ProjectIndex and MergedIndex shards.
    void merge(const void* tu_index_data, std::size_t size);

    /// Save ProjectIndex and MergedIndex shards to disk.
    void save(llvm::StringRef index_dir);

    /// Load ProjectIndex and MergedIndex shards from disk.
    void load(llvm::StringRef index_dir);

    /// Check whether a file needs re-indexing (stale or missing shard).
    bool need_update(llvm::StringRef file_path);

    /// Store or replace the open file index for a server-level path_id.
    void set_open_file(std::uint32_t server_path_id,
                       llvm::StringRef file_path,
                       OpenFileIndex index);

    /// Remove the open file index and untrack project-level path_id.
    void remove_open_file(std::uint32_t server_path_id, llvm::StringRef file_path);

    /// Query relations (Definition, Reference, etc.) for a symbol at cursor.
    /// @param doc_text  Fallback text when the file has no open file index yet.
    std::vector<protocol::Location> query_relations(llvm::StringRef path,
                                                    std::uint32_t server_path_id,
                                                    const protocol::Position& position,
                                                    RelationKind kind,
                                                    const std::string* doc_text);

    /// Look up symbol info (hash, name, kind, range) at a cursor position.
    std::optional<SymbolInfo> lookup_symbol(const std::string& uri,
                                            llvm::StringRef path,
                                            std::uint32_t server_path_id,
                                            const protocol::Position& position,
                                            const std::string* doc_text);

    /// Find the definition location of a symbol by hash.
    std::optional<protocol::Location> find_definition_location(index::SymbolHash hash);

    /// Find a symbol's name and kind by hash.
    bool find_symbol_info(index::SymbolHash hash, std::string& name, SymbolKind& kind) const;

    /// Resolve a hierarchy item (from stored data or by position lookup).
    std::optional<SymbolInfo> resolve_hierarchy_item(const std::string& uri,
                                                     llvm::StringRef path,
                                                     std::uint32_t server_path_id,
                                                     const protocol::Range& range,
                                                     const std::optional<protocol::LSPAny>& data,
                                                     const std::string* doc_text);

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

    /// Direct access to ProjectIndex for background indexing.
    index::ProjectIndex& project_index_ref() {
        return project_index;
    }

private:
    /// Result of resolving a symbol at a cursor position.
    struct CursorHit {
        index::SymbolHash hash = 0;
        protocol::Range range{};
    };

    /// Resolve the symbol at (position), checking open file index first then
    /// falling back to MergedIndex.
    CursorHit resolve_cursor(llvm::StringRef path,
                             std::uint32_t server_path_id,
                             const protocol::Position& position,
                             const std::string* doc_text);

    /// Collect relations grouped by target symbol, across all index sources.
    void collect_grouped_relations(
        index::SymbolHash hash,
        RelationKind kind,
        llvm::DenseMap<index::SymbolHash, std::vector<protocol::Range>>& target_ranges);

    /// Collect unique target symbol hashes for a relation kind.
    void collect_unique_targets(index::SymbolHash hash,
                                RelationKind kind,
                                llvm::SmallVectorImpl<index::SymbolHash>& targets);

private:
    PathPool& path_pool;

    /// Global symbol table and path pool shared across all TUs.
    index::ProjectIndex project_index;

    /// Per-file MergedIndex shards (keyed by project-level path_id).
    llvm::DenseMap<std::uint32_t, MergedIndexShard> merged_indices;

    /// In-memory indices for currently open files (keyed by server-level path_id).
    llvm::DenseMap<std::uint32_t, OpenFileIndex> open_file_indices;

    /// Project-level path_ids of open files, used to skip stale MergedIndex
    /// shards when fresher OpenFileIndex data is available.
    llvm::DenseSet<std::uint32_t> open_proj_path_ids;
};

}  // namespace clice

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "eventide/ipc/lsp/position.h"
#include "eventide/ipc/lsp/protocol.h"
#include "eventide/serde/serde/raw_value.h"
#include "index/merged_index.h"
#include "index/project_index.h"
#include "semantic/relation_kind.h"
#include "semantic/symbol_kind.h"
#include "support/path_pool.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
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
};

/// Information about a symbol at a given position.
struct SymbolInfo {
    /// Unique hash identifying this symbol across the project.
    index::SymbolHash hash = 0;

    /// Human-readable symbol name.
    std::string name;

    /// Symbol kind (function, class, variable, etc.).
    SymbolKind kind;

    /// URI of the file containing this symbol.
    std::string uri;

    /// Source range of the symbol's identifier.
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
    /// Also tracks the project-level path_id for cross-file query filtering.
    void set_open_file(std::uint32_t server_path_id,
                       llvm::StringRef file_path,
                       OpenFileIndex ofi);

    /// Remove the open file index and untrack project-level path_id.
    void remove_open_file(std::uint32_t server_path_id, llvm::StringRef file_path);

    /// Query relations (Definition, Reference, etc.) for a symbol at cursor.
    /// @param doc_text  Fallback text for position mapping when the file has no
    ///                  open file index yet (may be nullptr for non-open files).
    et::serde::RawValue query_relations(llvm::StringRef path,
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

    /// Hierarchy & workspace queries (return serialized JSON).
    et::serde::RawValue find_incoming_calls(index::SymbolHash hash);
    et::serde::RawValue find_outgoing_calls(index::SymbolHash hash);
    et::serde::RawValue find_supertypes(index::SymbolHash hash);
    et::serde::RawValue find_subtypes(index::SymbolHash hash);
    et::serde::RawValue search_symbols(llvm::StringRef query, std::size_t max_results = 100);

    static protocol::SymbolKind to_lsp_symbol_kind(SymbolKind kind);
    static protocol::CallHierarchyItem build_call_hierarchy_item(const SymbolInfo& info);
    static protocol::TypeHierarchyItem build_type_hierarchy_item(const SymbolInfo& info);

    index::ProjectIndex& project_index_ref() { return project_index; }

private:
    PathPool& path_pool;
    index::ProjectIndex project_index;
    llvm::DenseMap<std::uint32_t, index::MergedIndex> merged_indices;
    llvm::DenseMap<std::uint32_t, OpenFileIndex> open_file_indices;
    llvm::DenseSet<std::uint32_t> open_proj_path_ids;

    /// Result of resolving a symbol at a cursor position.
    struct CursorHit {
        index::SymbolHash hash = 0;
        index::Range range{};
        /// PositionMapper bound to the content used for lookup (for converting range back).
        std::optional<lsp::PositionMapper> mapper;
    };

    /// Shared logic for query_relations and lookup_symbol: resolve the symbol
    /// at (position) in the given file, checking open file index first then
    /// falling back to MergedIndex.
    CursorHit resolve_cursor(llvm::StringRef path,
                             std::uint32_t server_path_id,
                             const protocol::Position& position,
                             const std::string* doc_text);
};

}  // namespace clice

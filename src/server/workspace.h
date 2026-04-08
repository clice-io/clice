#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>

#include "command/command.h"
#include "eventide/ipc/lsp/position.h"
#include "eventide/ipc/lsp/protocol.h"
#include "index/merged_index.h"
#include "index/project_index.h"
#include "semantic/relation_kind.h"
#include "server/artifact_cache.h"
#include "server/compile_graph.h"
#include "server/config.h"
#include "support/path_pool.h"
#include "syntax/dependency_graph.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"

namespace clice {

namespace et = eventide;
namespace protocol = et::ipc::protocol;
namespace lsp = et::ipc::lsp;

/// Context for compiling a header file that lacks its own CDB entry.
struct HeaderFileContext {
    std::uint32_t host_path_id;   ///< Source file acting as host.
    std::string preamble_path;    ///< Path to generated preamble file on disk.
    std::uint64_t preamble_hash;  ///< Hash of preamble content for staleness.
};

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

/// Active PCH state for an open file.  References an entry in ArtifactCache.
/// This is the "hot" layer — tracks what's currently in use and being built.
struct PCHState {
    ArtifactKey artifact_key = 0;               ///< Key into ArtifactCache.
    std::uint32_t bound = 0;                    ///< Preamble byte boundary.
    std::shared_ptr<eventide::event> building;  ///< Non-null during build.
};

/// All persistent, project-wide state derived from files on disk.
///
/// Design principle: open files are never depended upon by other files.
/// Dependencies always point to disk files.  This enforces a clean two-layer
/// architecture:
///   - Global layer (Workspace): tracks disk truth, shared by all files
///   - Per-file layer (Session): tracks buffer truth, isolated per TU
///
/// Workspace is the single source of truth for:
///   - dependency relationships (include graph, module DAG)
///   - compilation artifacts shared across files (PCH/PCM caches)
///   - symbol index (ProjectIndex + per-file MergedIndex shards)
///   - compilation database and configuration
///
/// Workspace is NEVER modified by unsaved buffer content.  The only mutation
/// paths are:
///   - Initialization  (load_workspace at startup)
///   - didSave         (on_file_saved: rescan disk, cascade invalidation)
///   - Background index (merge TUIndex results from stateless workers)
struct Workspace {
    CliceConfig config;
    CompilationDatabase cdb;

    PathPool path_pool;

    /// Include relationships between files on disk (#include edges).
    /// Built once at startup from CDB scan; updated incrementally on didSave.
    DependencyGraph dep_graph;

    /// C++20 module compilation ordering DAG.
    /// Lazily resolves module dependencies; updated on didSave via cascade.
    std::unique_ptr<CompileGraph> compile_graph;

    /// Reverse mapping: file path_id → module name (e.g. "std", "foo.bar").
    /// Built from dep_graph at startup; updated on didSave when module
    /// declarations change.
    llvm::DenseMap<std::uint32_t, std::string> path_to_module;

    /// Content-addressed compilation artifact cache (PCH and PCM files).
    /// This is the "cold" storage layer with LRU eviction.
    ArtifactCache artifact_cache;

    /// Active PCH state per open file (keyed by path_id).
    /// References entries in artifact_cache.  Manages building coordination.
    llvm::DenseMap<std::uint32_t, PCHState> pch_active;

    /// Active PCM artifact keys per module source (keyed by path_id).
    /// Maps module source path_id to its ArtifactKey in artifact_cache.
    llvm::DenseMap<std::uint32_t, ArtifactKey> pcm_active;

    /// Global symbol table across all indexed translation units.
    index::ProjectIndex project_index;

    /// Per-file index shards from background indexing, keyed by project-level
    /// path_id.  Contains symbol occurrences, relations, and stored content
    /// for position mapping.
    llvm::DenseMap<std::uint32_t, MergedIndexShard> merged_indices;

    /// Called when a file is saved to disk.  Cascades invalidation through
    /// compile_graph and clears affected PCM caches.
    /// Returns path_ids of all files dirtied by the cascade.
    llvm::SmallVector<std::uint32_t> on_file_saved(std::uint32_t path_id);

    /// Called when a file is closed.  Notifies compile_graph if this file
    /// is a module unit so dependents can be re-evaluated on next compile.
    void on_file_closed(std::uint32_t path_id);

    /// Load artifact cache from disk.
    void load_cache();
    /// Save artifact cache to disk.
    void save_cache();
    /// Remove stale artifact files older than max_age_days.
    void cleanup_cache(int max_age_days = 7);
    /// Build path_to_module reverse mapping from dep_graph.
    void build_module_map();
    /// Fill PCM paths for all built modules, excluding exclude_path_id.
    void fill_pcm_deps(std::unordered_map<std::string, std::string>& pcms,
                       std::uint32_t exclude_path_id = UINT32_MAX) const;
    /// Cancel all in-flight compilations.
    void cancel_all();
};

}  // namespace clice

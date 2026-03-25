#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "compile/command.h"
#include "support/path_pool.h"
#include "syntax/include_resolver.h"
#include "syntax/scan.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"

namespace clice {

class DependencyGraph {
public:
    /// Conditional flag: bit 31 marks an include inside #ifdef/#if.
    constexpr static std::uint32_t CONDITIONAL_FLAG = 0x80000000u;

    /// Mask to extract the actual PathID from a flagged value.
    constexpr static std::uint32_t PATH_ID_MASK = 0x7FFFFFFFu;

    /// Key for per-(file, SearchConfig) include storage.
    struct IncludeKey {
        std::uint32_t path_id;
        std::uint32_t config_id;

        bool operator==(const IncludeKey&) const = default;
    };

    struct IncludeKeyInfo {
        static IncludeKey getEmptyKey() {
            return {~0u, ~0u};
        }

        static IncludeKey getTombstoneKey() {
            return {~0u - 1, ~0u - 1};
        }

        static unsigned getHashValue(const IncludeKey& key) {
            return llvm::DenseMapInfo<std::uint64_t>::getHashValue(
                (std::uint64_t(key.path_id) << 32) | key.config_id);
        }

        static bool isEqual(const IncludeKey& lhs, const IncludeKey& rhs) {
            return lhs == rhs;
        }
    };

    /// Register a module name -> PathID mapping.
    void add_module(llvm::StringRef module_name, std::uint32_t path_id);

    /// Look up the PathID that provides a given module.
    std::optional<std::uint32_t> lookup_module(llvm::StringRef module_name) const;

    /// Set the direct include list for a (file, config) pair.
    void set_includes(std::uint32_t path_id,
                      std::uint32_t config_id,
                      llvm::SmallVector<std::uint32_t> included_ids);

    /// Get direct includes for a specific (file, config) pair.
    llvm::ArrayRef<std::uint32_t> get_includes(std::uint32_t path_id,
                                               std::uint32_t config_id) const;

    /// Get the union of includes across all configs for a file.
    llvm::SmallVector<std::uint32_t> get_all_includes(std::uint32_t path_id) const;

    /// Number of files with include entries.
    std::size_t file_count() const;

    /// Number of module mappings.
    std::size_t module_count() const;

    /// Total number of include edges across all (file, config) pairs.
    std::size_t edge_count() const;

    /// Access the module name -> PathID mapping.
    const llvm::StringMap<std::uint32_t>& modules() const {
        return module_to_path;
    }

private:
    /// Module name -> PathID.
    llvm::StringMap<std::uint32_t> module_to_path;

    /// (PathID, ConfigID) -> list of directly included PathIDs.
    /// Each PathID may have bit 31 set to indicate conditional include.
    llvm::DenseMap<IncludeKey, llvm::SmallVector<std::uint32_t>, IncludeKeyInfo> includes;

    /// Track which files have any include entries (for file_count).
    llvm::DenseMap<std::uint32_t, llvm::SmallVector<std::uint32_t>> file_configs;
};

/// A (file, search-config) pair used to track per-wave work items.
struct WaveEntry {
    std::uint32_t path_id;
    std::uint32_t config_id;
};

/// Detailed report from a dependency scan.
struct ScanReport {
    /// Timing in milliseconds.
    std::int64_t elapsed_ms = 0;

    /// File counts.
    std::size_t source_files = 0;  // Files from CDB (translation units).
    std::size_t header_files = 0;  // Files discovered via include scanning.
    std::size_t total_files = 0;   // source_files + header_files.

    /// Include edge counts.
    std::size_t total_edges = 0;          // Total include edges.
    std::size_t conditional_edges = 0;    // Edges inside #if/#ifdef.
    std::size_t unconditional_edges = 0;  // Edges not inside conditionals.

    /// Include resolution.
    std::size_t includes_found = 0;     // Total #include directives seen.
    std::size_t includes_resolved = 0;  // Successfully resolved to a file.

    /// Module info.
    std::size_t modules = 0;

    /// BFS wave count.
    std::size_t waves = 0;

    /// Wall-clock time per phase (milliseconds, summed across waves).
    std::int64_t phase1_ms = 0;       // Read + scan (parallel on thread pool).
    std::int64_t phase2_ms = 0;       // Include resolution (stat calls).
    std::int64_t phase3_ms = 0;       // Graph building (single-threaded).
    std::int64_t config_ms = 0;       // Config extraction (one-time, total).
    std::int64_t prewarm_ms = 0;      // Toolchain pre-warm subset.
    std::int64_t config_loop_ms = 0;  // lookup + extract_search_config loop.
    std::int64_t dir_cache_ms = 0;    // Dir cache pre-population (overlapped with Phase 1).

    /// Cumulative I/O time across all threads/files (microseconds).
    /// These are sums of per-file durations — will exceed wall-clock time
    /// when work is parallelized across threads.
    std::int64_t read_us = 0;  // File read (cumulative across threads).
    std::int64_t scan_us = 0;  // Lexer scan (cumulative across threads).
    std::int64_t fs_us = 0;    // Filesystem ops (readdir calls).

    /// Filesystem call counts.
    std::size_t dir_listings = 0;        // Actual readdir() calls (dir cache misses).
    std::size_t dir_hits = 0;            // Directory cache hits (no syscall).
    std::size_t fs_lookups = 0;          // Total file existence lookups.
    std::size_t include_cache_hits = 0;  // Include resolution cache hits (skipped resolve).
    std::size_t scan_cache_hits = 0;     // Scan result cache hits (skipped I/O + lexer).

    /// Per-wave timing breakdown for cold start analysis.
    struct WaveStats {
        std::size_t files = 0;            // Files processed in this wave.
        std::int64_t phase1_ms = 0;       // Read + scan (parallel).
        std::int64_t phase2_ms = 0;       // Include resolution (serial).
        std::size_t next_files = 0;       // Files discovered for next wave.
        std::size_t prefetch_count = 0;   // Prefetch tasks launched during Phase 2.
        std::size_t dir_listings = 0;     // readdir() calls in this wave.
        std::size_t dir_hits = 0;         // Dir cache hits in this wave.
        std::size_t cache_hits = 0;       // Scan cache hits in this wave.
    };

    std::vector<WaveStats> wave_stats;

    /// Unresolved includes: (header_name, includer_path).
    struct UnresolvedInclude {
        std::string header;
        std::string includer;
        bool is_angled = false;
        bool conditional = false;
    };

    std::vector<UnresolvedInclude> unresolved;
};

/// Persistent cache that can be reused across successive scan calls.
/// Holding onto this between incremental re-scans eliminates repeated
/// readdir() calls, angled-include resolution, and file I/O on warm runs.
///
/// Thread safety: not thread-safe; callers must serialise scan calls.
///
/// Invalidation: callers must clear (or discard) this cache whenever the
/// compilation database or filesystem state changes.
struct ScanCache {
    /// Directory listing cache: dir path → set of filenames.
    DirListingCache dir_cache;

    /// Angled-include resolution cache: (config_id bytes + header) → path_id.
    /// path_id values are valid only for the PathPool used during the scan
    /// that populated this cache.  If PathPool is reset between scans, clear
    /// this cache too (or pass nullptr to scan_dependency_graph).
    llvm::StringMap<std::uint32_t> include_cache;

    /// Lexer scan result cache: path_id → ScanResult.
    /// Populated on the first scan of each file.  On subsequent calls the
    /// worker-thread file read and lexer scan are skipped entirely, making
    /// warm-run Phase 1 effectively free.
    /// Invalidate per-entry when a file changes on disk.
    llvm::DenseMap<std::uint32_t, ScanResult> scan_results;

    // ── Config extraction cache ──────────────────────────────────────────
    // Populated during the first scan and reused on all subsequent calls
    // when the compilation database has not changed.

    /// Files grouped by unique CompilationInfo pointer (context).
    /// path_ids are valid for the persistent PathPool.
    llvm::DenseMap<const void*, llvm::SmallVector<std::uint32_t>> context_groups;

    /// Context pointer → dense config_id (index into configs).
    llvm::DenseMap<const void*, std::uint32_t> context_to_config_id;

    /// Per-config search configuration (reused across scans).
    llvm::DenseMap<std::uint32_t, SearchConfig> configs;

    /// Pre-built initial wave (wave 0): all source files with their config IDs.
    std::vector<WaveEntry> initial_wave;
};

/// Run the wavefront BFS scan over all files in the compilation database.
/// Internally creates a local event loop for async I/O (file reads via worker
/// thread pool, stat calls via libuv). Blocks until the scan is complete.
///
/// @param cache  Optional persistent cache. When non-null and pre-populated,
///               avoids repeated readdir() and include-resolution work across
///               successive calls.  PathPool must NOT be reset between calls
///               when a persistent cache is used (path_id values must remain stable).
ScanReport scan_dependency_graph(CompilationDatabase& cdb,
                                 const std::vector<UpdateInfo>& updates,
                                 PathPool& path_pool,
                                 DependencyGraph& graph,
                                 ScanCache* cache = nullptr);

}  // namespace clice

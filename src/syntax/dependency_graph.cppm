module;

#include "modules/prelude.h"

module clice:syntax.dependency_graph;

import :command.command;
import :syntax.include_resolver;
import :syntax.scan;
import :vfs.file_table;

namespace clice {

/// One include edge: the included file and whether the directive sits
/// inside a preprocessor conditional (#if/#ifdef).
struct IncludeEdge {
    Fid fid;
    bool conditional = false;
};

/// How the scan reached a file: the scan group whose command resolves its
/// includes, the search directory it was found in (`#include_next`
/// resumes after it), and whether it is a unit of the build rather than
/// an included file.
struct ScanContext {
    std::uint32_t group;
    std::optional<unsigned> found_dir_idx;
    bool unit = false;
};

class DependencyGraph {
public:
    /// Key for per-(file, SearchConfig) include storage.
    struct IncludeKey {
        Fid path_id;
        std::uint32_t config_id;

        bool operator==(const IncludeKey&) const = default;
    };

    struct IncludeKeyInfo {
        static unsigned getHashValue(const IncludeKey& key) {
            return llvm::DenseMapInfo<std::uint64_t>::getHashValue(
                (std::uint64_t(key.path_id.raw) << 32) | key.config_id);
        }

        static bool isEqual(const IncludeKey& lhs, const IncludeKey& rhs) {
            return lhs == rhs;
        }
    };

    /// Register a unit providing a module: module name -> fid.
    void add_module(llvm::StringRef module_name, Fid path_id);

    /// Re-register a file's module declaration after a save: the file
    /// leaves whatever module it declared before and, when `module_name`
    /// is non-empty, provides that one — so imports resolved between two
    /// full scans see the declaration the disk actually holds.
    void update_module_decl(Fid path_id, llvm::StringRef module_name);

    /// Look up all fids that provide a given module (may have multiple candidates).
    llvm::ArrayRef<Fid> lookup_module(llvm::StringRef module_name) const;

    /// The module a file provides to importers (see
    /// ScanResult::provided_module); empty for every other file. Borrowed from the graph:
    /// copy it before a suspension that could re-declare the file.
    llvm::StringRef module_of(Fid path_id) const;

    /// Set the direct include list for a (file, config) pair.
    void set_includes(Fid path_id,
                      std::uint32_t config_id,
                      llvm::SmallVector<IncludeEdge> included);

    /// Get direct includes for a specific (file, config) pair.
    llvm::ArrayRef<IncludeEdge> get_includes(Fid path_id, std::uint32_t config_id) const;

    /// Get the union of included fids across all configs for a file.
    llvm::SmallVector<Fid> get_all_includes(Fid path_id) const;

    /// Every file a compile of `unit` is expected to read, `unit` included:
    /// the include edges under every configuration, transitively, and the
    /// headers its commands force in.
    llvm::DenseSet<Fid> include_closure(Fid unit) const;

    /// How many directives of `includer` include `target`, under the
    /// configuration with the most: one edge per directive.
    std::uint32_t count_includes(Fid includer, Fid target) const;

    /// Erase every config's include list for a file. Incremental rescans
    /// clear first, then re-add one list per configuration.
    void clear_includes(Fid path_id);

    /// Build the reverse include map from the forward includes. A bulk
    /// scan fills the forward edges first and builds once; from then on
    /// set_includes() and clear_includes() keep the map current edge by
    /// edge.
    void build_reverse_map();

    /// Get the direct includers of a file (files that directly include path_id).
    llvm::ArrayRef<Fid> get_includers(Fid path_id) const;

    /// Record that a command of `unit` includes `header` ahead of the
    /// unit's text (`-include`). A command fact, not a directive in any
    /// file's text: it stays out of the include edges, so hosting and
    /// context synthesis, which cut a host's text at a directive, never
    /// walk it.
    void add_forced_include(Fid unit, Fid header);

    /// The units whose commands force `header` in.
    llvm::ArrayRef<Fid> get_forcing_units(Fid header) const;

    /// A scan group: the command its units resolve includes under.
    std::uint32_t add_group(const CommandRef& command);
    const CommandRef& group(std::uint32_t id) const;

    llvm::ArrayRef<CommandRef> groups() const {
        return scan_groups;
    }

    /// Record how the scan reached a file; a unit has one context per
    /// command, any other file the first one that reached it.
    void add_context(Fid path_id, ScanContext context);
    llvm::ArrayRef<ScanContext> contexts(Fid path_id) const;

    /// BFS upward through reverse edges to find all roots — files without
    /// includers: source files, and forced headers — that transitively
    /// include header_path_id.
    llvm::SmallVector<Fid, 4> find_host_sources(Fid header_path_id) const;

    /// The roots whose compiles read the file: find_host_sources(), with a
    /// forced header climbing on to the units that force it in.
    llvm::SmallVector<Fid, 4> find_readers(Fid path_id) const;

    /// BFS forward through include edges to find the shortest include chain
    /// from host_path_id to target_path_id.
    /// Returns [host, intermediate1, ..., target], or empty if no path exists.
    std::vector<Fid> find_include_chain(Fid host_path_id, Fid target_path_id) const;

    /// Whether the graph knows the file: it has include entries or is an
    /// include target.
    bool knows(Fid path_id) const {
        return file_configs.contains(path_id) || reverse_includes.contains(path_id);
    }

    /// Every file the graph knows: files with include entries plus files
    /// that only appear as include targets. Sorted so callers scan in a
    /// deterministic order. Requires build_reverse_map() to have run.
    llvm::SmallVector<Fid> all_files() const;

    /// Number of files with include entries.
    std::size_t file_count() const;

    /// Number of module mappings.
    std::size_t module_count() const;

    /// Total number of include edges across all (file, config) pairs.
    std::size_t edge_count() const;

    /// Access the module name -> fid mapping.
    const llvm::StringMap<llvm::SmallVector<Fid, 2>>& modules() const {
        return module_to_path;
    }

    /// Record the content facts of a file's scan: whether it holds module
    /// syntax (an import candidate, see ScanResult::has_module_syntax),
    /// and the directive stream the edges were resolved from.
    void record_scan(Fid path_id, const ScanResult& scan);

    /// Drop them, for a file gone from disk.
    void forget_scan(Fid path_id);

    /// Whether the scan saw the file with this directive stream: then its
    /// edges describe a text with exactly these directives.
    bool scanned(Fid path_id, std::uint64_t directives_hash) const;

    /// Whether any file holds module syntax. The names stay unknown — an
    /// import's names are macro-expanded — but whether there is one is
    /// lexical truth: no macro can produce an import directive
    /// ([cpp.pre]).
    bool has_import_candidates() const {
        return !import_candidates.empty();
    }

    /// Whether a compile of `path_id`'s scanned text reads an import
    /// candidate: the file itself, what it includes, and its forced
    /// includes, transitively.
    bool reaches_import(Fid path_id) const;

    /// Forget the graph but keep its quick scans: they are content-keyed,
    /// so a rebuild over the same files reuses every one still current.
    void reset();

    /// The quick scans the graph was built from.
    QuickScanCache quick_scans;

private:
    /// Module name -> fids (multiple candidates possible, e.g. different targets).
    llvm::StringMap<llvm::SmallVector<Fid, 2>> module_to_path;

    /// The inverse of module_to_path, maintained by the same two writers.
    llvm::DenseMap<Fid, std::string> module_by_path;

    /// See record_scan().
    llvm::DenseSet<Fid> import_candidates;
    llvm::DenseMap<Fid, std::uint64_t> scanned_directives;

    /// (fid, ConfigID) -> directly included files.
    llvm::DenseMap<IncludeKey, llvm::SmallVector<IncludeEdge>, IncludeKeyInfo> includes;

    /// Track which files have any include entries (for file_count).
    llvm::DenseMap<Fid, llvm::SmallVector<std::uint32_t>> file_configs;

    /// Reverse include map: fid -> files that directly include it.
    /// Populated by build_reverse_map().
    llvm::DenseMap<Fid, llvm::SmallVector<Fid, 4>> reverse_includes;

    /// Unit -> headers its commands force in, and the inverse, sorted.
    llvm::DenseMap<Fid, llvm::SmallVector<Fid, 1>> forced_includes;
    llvm::DenseMap<Fid, llvm::SmallVector<Fid, 4>> forcing_units;

    /// See add_group() and add_context().
    llvm::SmallVector<CommandRef> scan_groups;
    llvm::DenseMap<Fid, llvm::SmallVector<ScanContext, 1>> scan_contexts;

    /// Whether build_reverse_map() ran, so edge updates maintain the map.
    bool reverse_built = false;

    /// Record `includer` among `target`'s includers, and drop it.
    void link(Fid includer, Fid target);
    void unlink(Fid includer, Fid target);

    /// The roots above `path_id`, climbing from forced headers to their
    /// units when `through_forced` is set.
    llvm::SmallVector<Fid, 4> find_roots(Fid path_id, bool through_forced) const;

    /// Walk the include closure of `unit` (see include_closure), adding
    /// each file to `seen`, until `stop` holds for one; returns whether it
    /// did.
    bool walk_closure(Fid unit,
                      llvm::DenseSet<Fid>& seen,
                      llvm::function_ref<bool(Fid)> stop) const;
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

    /// Phase 2 breakdown (microseconds, single-threaded).
    std::int64_t p2_resolve_us = 0;  // resolve_include() calls.

    /// Filesystem call counts.
    std::size_t dir_listings = 0;        // Actual readdir() calls (dir cache misses).
    std::size_t dir_hits = 0;            // Directory cache hits (no syscall).
    std::size_t fs_lookups = 0;          // Total file existence lookups.
    std::size_t include_cache_hits = 0;  // Include resolution cache hits (skipped resolve).
    std::size_t scan_cache_hits = 0;     // Scan result cache hits (skipped I/O + lexer).

    /// Per-wave timing breakdown for cold start analysis.
    struct WaveStats {
        std::size_t files = 0;           // Files processed in this wave.
        std::int64_t phase1_ms = 0;      // Read + scan (parallel).
        std::int64_t phase2_ms = 0;      // Include resolution (serial).
        std::size_t next_files = 0;      // Files discovered for next wave.
        std::size_t prefetch_count = 0;  // Prefetch tasks launched during Phase 2.
        std::size_t dir_listings = 0;    // readdir() calls in this wave.
        std::size_t dir_hits = 0;        // Dir cache hits in this wave.
        std::size_t cache_hits = 0;      // Scan cache hits in this wave.
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

/// Run the wavefront BFS scan from `units` — the translation units to scan,
/// each with its effective (rules-applied) command. Internally creates a
/// local event loop for async I/O (file reads via worker thread pool, stat
/// calls via libuv). Blocks until the scan is complete.
ScanReport scan_dependency_graph(CompilationDatabase& cdb,
                                 DependencyGraph& graph,
                                 llvm::ArrayRef<CommandRef> units);

/// Bring a file whose disk content changed back in step, by the same
/// per-file step as the full scan: under every context the scan reached
/// it by, its include edges, module declaration and module syntax follow
/// the new bytes, and files it reaches for the first time are scanned
/// too. A file the scan never reached stays out of the graph.
void rescan_dependency_graph(CompilationDatabase& cdb, DependencyGraph& graph, Fid path_id);

}  // namespace clice

module;

#include "modules/prelude.h"

module clice:syntax.dependency_graph;

import :command.database;
import :command.table;
import :command.toolchain;
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
    /// Register a unit providing a module: module name -> fid. `internal`
    /// marks an internal partition (ScanResult::is_internal_partition).
    void add_module(llvm::StringRef module_name, Fid path_id, bool internal = false);

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

    /// Whether the module the file provides is an internal partition.
    bool internal_partition(Fid path_id) const {
        return internal_partitions.contains(path_id);
    }

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
    /// file's text: it stays out of the include edges, as no cut of the
    /// unit's text reproduces it.
    void add_forced_include(Fid unit, Fid header);

    /// The units whose commands force `header` in.
    llvm::ArrayRef<Fid> get_forcing_units(Fid header) const;

    /// The headers `unit`'s commands force in, in the order they do.
    llvm::ArrayRef<Fid> get_forced_includes(Fid unit) const;

    /// A scan group: the command its units resolve includes under.
    std::uint32_t add_group(const CommandRef& command);
    const CommandRef& group(std::uint32_t id) const;

    llvm::ArrayRef<CommandRef> groups() const {
        return scan_groups;
    }

    /// Record how the scan reached a file; a unit has one context per
    /// scan group its commands fall in, any other file the first one that
    /// reached it.
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

    /// The providers of internal partitions, maintained with module_by_path.
    llvm::DenseSet<Fid> internal_partitions;

    /// See record_scan().
    llvm::DenseSet<Fid> import_candidates;
    llvm::DenseMap<Fid, std::uint64_t> scanned_directives;

    /// (fid, scan group) -> directly included files.
    llvm::DenseMap<std::pair<Fid, std::uint32_t>, llvm::SmallVector<IncludeEdge>> includes;

    /// Track which files have any include entries (for file_count).
    llvm::DenseMap<Fid, llvm::SmallVector<std::uint32_t>> file_configs;

    /// Reverse include map: fid -> files that directly include it.
    /// Populated by build_reverse_map().
    llvm::DenseMap<Fid, llvm::SmallVector<Fid, 4>> reverse_includes;

    /// Unit -> headers its commands force in, in command order, and the
    /// inverse, sorted.
    llvm::DenseMap<Fid, llvm::SmallVector<Fid, 1>> forced_includes;
    llvm::DenseMap<Fid, llvm::SmallVector<Fid, 4>> forcing_units;

    /// See add_group() and add_context().
    llvm::SmallVector<CommandRef> scan_groups;
    llvm::DenseMap<Fid, llvm::SmallVector<ScanContext, 1>> scan_contexts;

    /// Whether build_reverse_map() ran, so edge updates maintain the map.
    bool reverse_built = false;

    /// Drop `includer` from `target`'s includers.
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

/// What a dependency scan did, for the startup log.
struct ScanReport {
    std::int64_t elapsed_ms = 0;

    /// Files scanned: the units from the CDB, and all files including the
    /// headers the scan reached.
    std::size_t source_files = 0;
    std::size_t total_files = 0;

    /// #include directives resolved to a file, one include edge each.
    std::size_t includes_resolved = 0;

    /// An include no search found, as its includer wrote it.
    struct Unresolved {
        Fid includer;
        std::string name;
        bool angled = false;
    };

    /// The includes not resolved, in scan order; a file scanned under
    /// several groups lists one again for each that misses it.
    std::vector<Unresolved> unresolved;

    /// Files whose lexical scan was reused instead of read and lexed again.
    std::size_t scan_cache_hits = 0;

    std::size_t waves = 0;
};

/// Run the wavefront BFS scan from `units` — the translation units to scan,
/// each with its effective (rules-applied) command. Internally creates a
/// local event loop for async I/O (file reads via worker thread pool, stat
/// calls via libuv). Blocks until the scan is complete.
ScanReport scan_dependency_graph(CompilationDatabase& cdb,
                                 Toolchain& toolchain,
                                 DependencyGraph& graph,
                                 llvm::ArrayRef<CommandRef> units);

/// Bring a file whose disk content changed back in step, by the same
/// per-file step as the full scan: under every context the scan reached
/// it by, its include edges, module declaration and module syntax follow
/// the new bytes, and files it reaches for the first time are scanned
/// too. A file the scan never reached stays out of the graph.
void rescan_dependency_graph(CompilationDatabase& cdb,
                             Toolchain& toolchain,
                             DependencyGraph& graph,
                             Fid path_id);

}  // namespace clice

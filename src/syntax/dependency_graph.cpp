module;

#include "modules/prelude.h"

#include "support/logging.macros.h"

module clice;

import :command.search_config;
import :support.logging;
import :support.timer;
import :syntax.dependency_graph;
import :syntax.include_resolver;
import :syntax.scan;
import :vfs.file_table;

namespace clice {

// DependencyGraph implementation

void DependencyGraph::add_module(llvm::StringRef module_name, Fid path_id, bool internal) {
    auto& ids = module_to_path[module_name];
    if(llvm::find(ids, path_id) == ids.end()) {
        ids.push_back(path_id);
    }
    module_by_path[path_id] = module_name.str();
    if(internal) {
        internal_partitions.insert(path_id);
    } else {
        internal_partitions.erase(path_id);
    }
}

void DependencyGraph::update_module_decl(Fid path_id, llvm::StringRef module_name) {
    // Re-declaring the unchanged name is a no-op: providers are selected
    // by list order, so an erase-and-append would silently reselect
    // among a duplicated name's providers without any cascade.
    if(!module_name.empty() && llvm::is_contained(lookup_module(module_name), path_id)) {
        // The file may still be listed under a name another configuration
        // scanned it as; the declaration the save met is the one it
        // provides now.
        module_by_path[path_id] = module_name.str();
        return;
    }
    // An emptied name stays behind as an empty provider list — lookup
    // treats it the same as absent.
    for(auto& entry: module_to_path) {
        llvm::erase(entry.getValue(), path_id);
    }
    module_by_path.erase(path_id);
    internal_partitions.erase(path_id);
    if(!module_name.empty()) {
        add_module(module_name, path_id);
    }
}

llvm::StringRef DependencyGraph::module_of(Fid path_id) const {
    auto it = module_by_path.find(path_id);
    return it == module_by_path.end() ? llvm::StringRef() : llvm::StringRef(it->second);
}

llvm::ArrayRef<Fid> DependencyGraph::lookup_module(llvm::StringRef module_name) const {
    auto it = module_to_path.find(module_name);
    if(it != module_to_path.end()) {
        return it->second;
    }
    return {};
}

/// Insert into a sorted list, once.
static void insert_sorted(llvm::SmallVectorImpl<Fid>& list, Fid fid) {
    auto it = llvm::lower_bound(list, fid);
    if(it == list.end() || *it != fid) {
        list.insert(it, fid);
    }
}

void DependencyGraph::unlink(Fid includer, Fid target) {
    auto found = reverse_includes.find(target);
    if(found == reverse_includes.end()) {
        return;
    }
    auto& includers = found->second;
    auto it = llvm::lower_bound(includers, includer);
    if(it != includers.end() && *it == includer) {
        includers.erase(it);
    }
    if(includers.empty()) {
        reverse_includes.erase(found);
    }
}

void DependencyGraph::set_includes(Fid path_id,
                                   std::uint32_t config_id,
                                   llvm::SmallVector<IncludeEdge> included) {
    std::pair key{path_id, config_id};
    if(reverse_built) {
        auto old = includes.find(key);
        if(old != includes.end()) {
            auto dropped = std::move(old->second);
            includes.erase(old);
            auto still_included = get_all_includes(path_id);
            for(auto edge: dropped) {
                if(!llvm::is_contained(still_included, edge.fid) &&
                   llvm::none_of(included,
                                 [&](IncludeEdge kept) { return kept.fid == edge.fid; })) {
                    unlink(path_id, edge.fid);
                }
            }
        }
        for(auto edge: included) {
            insert_sorted(reverse_includes[edge.fid], path_id);
        }
    }
    includes[key] = std::move(included);
    auto& configs = file_configs[path_id];
    if(std::find(configs.begin(), configs.end(), config_id) == configs.end()) {
        configs.push_back(config_id);
    }
}

llvm::ArrayRef<IncludeEdge> DependencyGraph::get_includes(Fid path_id,
                                                          std::uint32_t config_id) const {
    auto it = includes.find(std::pair{path_id, config_id});
    if(it != includes.end()) {
        return it->second;
    }
    return {};
}

llvm::SmallVector<Fid> DependencyGraph::get_all_includes(Fid path_id) const {
    llvm::DenseSet<Fid> seen;
    llvm::SmallVector<Fid> result;

    auto fc_it = file_configs.find(path_id);
    if(fc_it == file_configs.end()) {
        return result;
    }

    for(auto config_id: fc_it->second) {
        auto it = includes.find(std::pair{path_id, config_id});
        if(it != includes.end()) {
            for(auto edge: it->second) {
                if(seen.insert(edge.fid).second) {
                    result.push_back(edge.fid);
                }
            }
        }
    }
    return result;
}

llvm::SmallVector<Fid> DependencyGraph::all_files() const {
    llvm::SmallVector<Fid> files;
    files.reserve(file_configs.size() + reverse_includes.size());
    for(auto& [path_id, configs]: file_configs) {
        files.push_back(path_id);
    }
    for(auto& [path_id, includers]: reverse_includes) {
        files.push_back(path_id);
    }
    llvm::sort(files);
    files.erase(llvm::unique(files), files.end());
    return files;
}

std::size_t DependencyGraph::file_count() const {
    return file_configs.size();
}

std::size_t DependencyGraph::edge_count() const {
    std::size_t count = 0;
    for(auto& [key, ids]: includes) {
        count += ids.size();
    }
    return count;
}

void DependencyGraph::clear_includes(Fid path_id) {
    auto it = file_configs.find(path_id);
    if(it == file_configs.end()) {
        return;
    }
    for(auto config_id: it->second) {
        auto key = includes.find(std::pair{path_id, config_id});
        if(key == includes.end()) {
            continue;
        }
        if(reverse_built) {
            for(auto edge: key->second) {
                unlink(path_id, edge.fid);
            }
        }
        includes.erase(key);
    }
    file_configs.erase(it);
}

void DependencyGraph::build_reverse_map() {
    reverse_built = true;
    reverse_includes.clear();
    for(auto& [key, ids]: includes) {
        for(auto edge: ids) {
            reverse_includes[edge.fid].push_back(key.first);
        }
    }
    // One includer appears once per configuration including the file, and
    // a widely included header has thousands of includers: deduplicate by
    // sorting, not by a linear search per edge.
    for(auto& [path_id, includers]: reverse_includes) {
        llvm::sort(includers);
        includers.erase(llvm::unique(includers), includers.end());
    }
}

llvm::ArrayRef<Fid> DependencyGraph::get_includers(Fid path_id) const {
    auto it = reverse_includes.find(path_id);
    if(it != reverse_includes.end()) {
        return it->second;
    }
    return {};
}

llvm::DenseSet<Fid> DependencyGraph::include_closure(Fid unit) const {
    llvm::DenseSet<Fid> closure;
    walk_closure(unit, closure, [](Fid) { return false; });
    return closure;
}

bool DependencyGraph::walk_closure(Fid unit,
                                   llvm::DenseSet<Fid>& seen,
                                   llvm::function_ref<bool(Fid)> stop) const {
    seen.insert(unit);
    llvm::SmallVector<Fid, 64> queue{unit};
    auto visit = [&](Fid fid) {
        if(seen.insert(fid).second) {
            queue.push_back(fid);
        }
    };
    while(!queue.empty()) {
        auto current = queue.pop_back_val();
        if(stop(current)) {
            return true;
        }
        if(auto it = file_configs.find(current); it != file_configs.end()) {
            for(auto config_id: it->second) {
                for(auto edge: get_includes(current, config_id)) {
                    visit(edge.fid);
                }
            }
        }
        if(auto it = forced_includes.find(current); it != forced_includes.end()) {
            for(auto header: it->second) {
                visit(header);
            }
        }
    }
    return false;
}

std::uint32_t DependencyGraph::count_includes(Fid includer, Fid target) const {
    std::uint32_t most = 0;
    auto it = file_configs.find(includer);
    if(it == file_configs.end()) {
        return most;
    }
    for(auto config_id: it->second) {
        auto count = llvm::count_if(get_includes(includer, config_id),
                                    [&](IncludeEdge edge) { return edge.fid == target; });
        most = std::max(most, static_cast<std::uint32_t>(count));
    }
    return most;
}

void DependencyGraph::add_forced_include(Fid unit, Fid header) {
    if(auto& forced = forced_includes[unit]; !llvm::is_contained(forced, header)) {
        forced.push_back(header);
    }
    insert_sorted(forcing_units[header], unit);
}

llvm::ArrayRef<Fid> DependencyGraph::get_forcing_units(Fid header) const {
    auto it = forcing_units.find(header);
    if(it != forcing_units.end()) {
        return it->second;
    }
    return {};
}

llvm::ArrayRef<Fid> DependencyGraph::get_forced_includes(Fid unit) const {
    auto it = forced_includes.find(unit);
    if(it != forced_includes.end()) {
        return it->second;
    }
    return {};
}

std::uint32_t DependencyGraph::add_group(const CommandRef& command) {
    scan_groups.push_back(command);
    return static_cast<std::uint32_t>(scan_groups.size() - 1);
}

const CommandRef& DependencyGraph::group(std::uint32_t id) const {
    return scan_groups[id];
}

void DependencyGraph::record_scan(Fid path_id, const ScanResult& scan) {
    if(scan.has_module_syntax()) {
        import_candidates.insert(path_id);
    } else {
        import_candidates.erase(path_id);
    }
    scanned_directives[path_id] = scan.directives_hash;
}

void DependencyGraph::forget_scan(Fid path_id) {
    import_candidates.erase(path_id);
    scanned_directives.erase(path_id);
}

bool DependencyGraph::scanned(Fid path_id, std::uint64_t directives_hash) const {
    auto it = scanned_directives.find(path_id);
    return it != scanned_directives.end() && it->second == directives_hash;
}

void DependencyGraph::add_context(Fid path_id, ScanContext context) {
    scan_contexts[path_id].push_back(context);
}

llvm::ArrayRef<ScanContext> DependencyGraph::contexts(Fid path_id) const {
    auto it = scan_contexts.find(path_id);
    if(it != scan_contexts.end()) {
        return it->second;
    }
    return {};
}

bool DependencyGraph::reaches_import(Fid path_id) const {
    if(import_candidates.empty()) {
        return false;
    }
    llvm::DenseSet<Fid> seen;
    return walk_closure(path_id, seen, [&](Fid fid) { return import_candidates.contains(fid); });
}

void DependencyGraph::reset() {
    auto kept = std::move(quick_scans);
    *this = DependencyGraph();
    quick_scans = std::move(kept);
}

llvm::SmallVector<Fid, 4> DependencyGraph::find_roots(Fid path_id, bool through_forced) const {
    llvm::SmallVector<Fid, 4> result;
    llvm::DenseSet<Fid> visited;
    llvm::SmallVector<Fid, 16> queue;

    queue.push_back(path_id);
    visited.insert(path_id);

    while(!queue.empty()) {
        auto current = queue.pop_back_val();
        auto includers = get_includers(current);
        auto forcing = through_forced ? get_forcing_units(current) : llvm::ArrayRef<Fid>();
        if(includers.empty() && forcing.empty()) {
            // Nothing above it: a root. Exclude the starting file itself.
            if(current != path_id) {
                result.push_back(current);
            }
            continue;
        }
        for(auto parent: llvm::concat<const Fid>(includers, forcing)) {
            if(visited.insert(parent).second) {
                queue.push_back(parent);
            }
        }
    }

    return result;
}

llvm::SmallVector<Fid, 4> DependencyGraph::find_host_sources(Fid header_path_id) const {
    return find_roots(header_path_id, false);
}

llvm::SmallVector<Fid, 4> DependencyGraph::find_readers(Fid path_id) const {
    return find_roots(path_id, true);
}

std::vector<Fid> DependencyGraph::find_include_chain(Fid host_path_id, Fid target_path_id) const {
    if(host_path_id == target_path_id) {
        return {host_path_id};
    }

    // BFS: predecessor map for path reconstruction.
    llvm::DenseMap<Fid, Fid> prev;
    llvm::SmallVector<Fid, 16> queue;

    prev[host_path_id] = host_path_id;
    queue.push_back(host_path_id);

    bool found = false;
    while(!queue.empty() && !found) {
        llvm::SmallVector<Fid, 16> next_queue;
        for(auto current: queue) {
            for(auto child: get_all_includes(current)) {
                if(prev.find(child) == prev.end()) {
                    prev[child] = current;
                    if(child == target_path_id) {
                        found = true;
                        break;
                    }
                    next_queue.push_back(child);
                }
            }
            if(found) {
                break;
            }
        }
        queue = std::move(next_queue);
    }

    if(!found) {
        return {};
    }

    // Reconstruct path from target back to host.
    std::vector<Fid> chain;
    auto node = target_path_id;
    while(node != host_path_id) {
        chain.push_back(node);
        node = prev[node];
    }
    chain.push_back(host_path_id);
    std::reverse(chain.begin(), chain.end());
    return chain;
}

namespace {

/// A file's bytes as a scan read them: their lexical scan, and the
/// same-source {stat, hash} pairing them — the cold-start read doubles as
/// the workspace's first disk observation.
struct ScannedText {
    DiskObservation obs;
    std::shared_ptr<const ScanResult> result;
};

/// Read and lex the file at `path`, a stable pointer from the FileTable
/// that outlives the task; none when it cannot be read. Runs on the
/// thread pool.
std::optional<ScannedText> read_and_scan(const char* path) {
    auto observed = vfs::read_observed(path);
    if(!observed) {
        return std::nullopt;
    }
    return ScannedText{
        .obs = observed->obs,
        .result = std::make_shared<const ScanResult>(scan_quick(observed->content->getBuffer())),
    };
}

/// A resolved include: the file, and the search dir it was found in.
struct CachedInclude {
    /// Invalid = the include is known-unresolvable under this config.
    Fid path_id;
    std::optional<unsigned> found_dir_idx;
};

/// The module a unit provides to importers, empty for every other file.
struct ProvidedModule {
    std::string name;
    bool internal = false;
};

/// The per-file step of every scan, the full one and a single file's
/// rescan alike: a file's scan under each of its contexts becomes its
/// module syntax, a unit's forced includes, and its include edges. The
/// listings, the groups' search configurations and the angled-include
/// memo live as long as the operation, so they never serve a stale
/// filesystem.
struct FileScanner {
    FileScanner(CompilationDatabase& cdb, Toolchain& toolchain, DependencyGraph& graph) :
        cdb(cdb), toolchain(toolchain), graph(graph), files(cdb.files()), scope(files.dirs) {}

    /// Record `path_id`'s scan of the bytes hashing to `hash` under each
    /// of `contexts`, returning the modules its units' commands declare.
    /// A file it includes or forces in that the scan reaches for the first
    /// time gets its context and goes to `reach`.
    llvm::SmallVector<ProvidedModule, 1> record(Fid path_id,
                                                std::shared_ptr<const ScanResult> scan,
                                                std::uint64_t hash,
                                                llvm::ArrayRef<ScanContext> contexts,
                                                llvm::function_ref<void(Fid, ScanContext)> reach) {
        llvm::SmallVector<ProvidedModule, 1> declared;
        for(auto context: contexts) {
            if(auto module = record_under(path_id, context, scan, hash, reach);
               !module.name.empty()) {
                declared.push_back(std::move(module));
            }
        }
        return declared;
    }

    /// record() under one context; a unit's module declaration may find
    /// the disk moved and rebuild `scan` and `hash` from the bytes it read.
    ProvidedModule record_under(Fid path_id,
                                ScanContext context,
                                std::shared_ptr<const ScanResult>& scan,
                                std::uint64_t& hash,
                                llvm::function_ref<void(Fid, ScanContext)> reach) {
        auto reached = [&](Fid target, std::optional<unsigned> found_dir_idx) {
            if(graph.contexts(target).empty()) {
                ScanContext target_context{.group = context.group, .found_dir_idx = found_dir_idx};
                graph.add_context(target, target_context);
                reach(target, target_context);
            }
        };

        auto& search = search_of(context.group);
        ProvidedModule module;
        if(context.unit) {
            module = module_of(path_id, context, scan, hash);
            for(auto& header: forced_of(context.group)) {
                graph.add_forced_include(path_id, header.path_id);
                reached(header.path_id, header.found_dir_idx);
            }
        }
        graph.record_scan(path_id, *scan);

        // Quoted includes start from the directory the build reaches the
        // includer through, as clang's do.
        auto includer_spelling = files.spelling(path_id).parent();
        llvm::StringRef includer_dir = includer_spelling;
        auto* includer_listing = &scope.list(includer_dir);

        llvm::SmallVector<IncludeEdge> edges;
        edges.reserve(scan->includes.size());
        for(auto& include: scan->includes) {
            // An angled include resolves the same from every includer
            // under one group: resolve it once per scan. Most groups hold
            // one unit, so the memo is one map keyed by group and name.
            bool memoized = include.is_angled && !include.is_include_next;
            llvm::SmallString<80> key;
            if(memoized) {
                key.append(reinterpret_cast<const char*>(&context.group),
                           reinterpret_cast<const char*>(&context.group) + sizeof(std::uint32_t));
                key += include.path;
            }

            CachedInclude resolved;
            if(auto it = memoized ? angled.find(key) : angled.end(); it != angled.end()) {
                resolved = it->second;
            } else {
                auto found = resolve_include(include.path,
                                             include.is_angled,
                                             includer_listing,
                                             includer_dir,
                                             include.is_include_next,
                                             context.found_dir_idx,
                                             search.resolved,
                                             scope);
                resolved = found ? CachedInclude{.path_id = files.intern_spelled(
                                                     Spelling::absolute(found->path)),
                                                 .found_dir_idx = found->found_dir_idx}
                                 : CachedInclude{};
                if(memoized) {
                    angled.try_emplace(key, resolved);
                }
            }

            if(!resolved.path_id.valid()) {
                unresolved.push_back(
                    {.includer = path_id, .name = include.path, .angled = include.is_angled});
                continue;
            }
            includes_resolved += 1;
            edges.push_back({resolved.path_id, include.conditional});
            reached(resolved.path_id, resolved.found_dir_idx);
        }
        graph.set_includes(path_id, context.group, std::move(edges));
        return module;
    }

    struct Search {
        SearchConfig config;
        ResolvedSearchConfig resolved;
    };

    /// A group's search configuration against this operation's listings.
    Search& search_of(std::uint32_t group) {
        auto& search = searches[group];
        if(!search) {
            search = std::make_unique<Search>();
            search->config = toolchain.search_config(graph.group(group));
            search->resolved = resolve_search_config(search->config, scope);
        }
        return *search;
    }

    /// A group's forced includes, resolved; the compile reports the ones
    /// that resolve nowhere.
    llvm::ArrayRef<CachedInclude> forced_of(std::uint32_t group) {
        auto [it, inserted] = forced_cache.try_emplace(group);
        if(inserted) {
            auto& search = search_of(group);
            llvm::StringRef directory = cdb.commands().get(graph.group(group).command).directory;
            for(auto& name: search.config.forced_includes) {
                if(auto found = resolve_forced_include(name, directory, search.resolved, scope)) {
                    it->second.push_back({
                        .path_id = files.intern_spelled(Spelling::absolute(found->path)),
                        .found_dir_idx = found->found_dir_idx,
                    });
                }
            }
        }
        return it->second;
    }

    /// The module a unit provides to importers. A declaration inside
    /// preprocessor conditionals is beyond the lexical scan: a
    /// preprocessor run under the unit's own group command resolves it —
    /// only its flags (a define unguarding the declaration) can.
    ProvidedModule module_of(Fid path_id,
                             ScanContext context,
                             std::shared_ptr<const ScanResult>& scan,
                             std::uint64_t& hash) {
        if(scan->need_preprocess) {
            if(auto observed = vfs::read_observed(files.resolve(path_id))) {
                // The preprocessor must consume the bytes that produced
                // this scan. When the disk moved under the scan, the whole
                // result is rebuilt from the bytes actually read — includes
                // and module name out of one version, never a chimera of
                // two.
                if(observed->obs.hash != hash) {
                    files.disk.observe(path_id, observed->obs);
                    hash = observed->obs.hash;
                    scan = graph.quick_scans.scan_of(path_id, hash, observed->content->getBuffer());
                }
                auto& group = graph.group(context.group);
                auto rendered =
                    toolchain.render({path_id, group.command, group.input, CommandSource::CDBExact},
                                     cdb.spelling(path_id));
                auto declared = scan_module_decl(rendered,
                                                 cdb.commands().get(group.command).directory,
                                                 observed->content->getBuffer());
                return {declared.provided_module().str(), declared.is_internal_partition()};
            }
        }
        return {scan->provided_module().str(), scan->is_internal_partition()};
    }

    CompilationDatabase& cdb;
    Toolchain& toolchain;
    DependencyGraph& graph;
    FileTable& files;
    vfs::Scope scope;
    llvm::DenseMap<std::uint32_t, std::unique_ptr<Search>> searches;
    llvm::DenseMap<std::uint32_t, llvm::SmallVector<CachedInclude>> forced_cache;
    llvm::StringMap<CachedInclude> angled;

    /// For the scan report: #include directives resolved, and not.
    std::size_t includes_resolved = 0;
    std::vector<ScanReport::Unresolved> unresolved;
};

/// The scan from the build's units, wave by wave — breadth first, so a
/// header takes its context from an includer nearest a unit, the same one
/// every run. A file reached for the first time is read and lexed on the
/// thread pool from the moment it is reached, unless the quick scans still
/// pin its bytes: the units' reads run beside the search configurations,
/// a wave's beside the recording of the one before.
struct FullScan {
    /// The files one wave records, in the order they were reached: the
    /// units under every group their commands fall in, a header under the
    /// context it was first reached by.
    struct Wave {
        struct File {
            Fid fid;

            /// Its contexts: `contexts[first, first + count)`.
            std::uint32_t first = 0;
            std::uint32_t count = 0;

            /// Whether its scan is the quick scan of the bytes the disk
            /// still holds rather than a read.
            bool warm = false;
        };

        std::vector<File> files;
        std::vector<ScanContext> contexts;

        /// `files[i]`'s scan; a read's is there once `reads` has joined.
        std::vector<std::optional<ScannedText>> scans;
        kota::task_group<> reads;
    };

    FullScan(CompilationDatabase& cdb,
             Toolchain& toolchain,
             DependencyGraph& graph,
             ScanReport& report) :
        cdb(cdb), toolchain(toolchain), graph(graph), files(cdb.files()), report(report),
        scanner(cdb, toolchain, graph) {}

    /// Add `fid`, reached under `contexts`, to `wave`.
    void add(Wave& wave, Fid fid, llvm::ArrayRef<ScanContext> contexts) {
        auto& file = wave.files.emplace_back(Wave::File{
            .fid = fid,
            .first = static_cast<std::uint32_t>(wave.contexts.size()),
            .count = static_cast<std::uint32_t>(contexts.size()),
        });
        wave.contexts.insert(wave.contexts.end(), contexts.begin(), contexts.end());
        if(wave.scans.emplace_back(warm(fid))) {
            report.scan_cache_hits += 1;
            file.warm = true;
            return;
        }
        wave.reads.spawn(read_into(wave.scans, wave.scans.size() - 1, files.resolve(fid).data()));
    }

    static kota::task<> read_into(std::vector<std::optional<ScannedText>>& scans,
                                  std::size_t index,
                                  const char* path) {
        auto scanned = co_await kota::queue([path] { return read_and_scan(path); });
        scans[index] = std::move(scanned);
    }

    /// The quick scan of `fid`'s bytes when a live stat the shared table's
    /// pair confirms pins them: a rescan skips the read and the lex of
    /// every unchanged file, at the cost of one stat.
    std::optional<ScannedText> warm(Fid fid) {
        auto status = statuses.status(files.resolve(fid));
        if(!status) {
            return std::nullopt;
        }
        auto hash = files.disk.cached_hash(fid, status->stamp);
        if(!hash) {
            return std::nullopt;
        }
        auto it = graph.quick_scans.results.find({fid, *hash});
        if(it == graph.quick_scans.results.end()) {
            return std::nullopt;
        }
        return ScannedText{
            .obs = {.stamp = status->stamp, .hash = *hash, .paired = true, .reliable = true},
            .result = it->second,
        };
    }

    /// List every search dir and every directory quoted includes of source
    /// files start from on the thread pool, into the scanner's listings —
    /// a listing the cache can still confirm skips the readdir; its one
    /// validation stat happens lazily at first use. Returns how many.
    std::size_t list_directories(kota::task_group<>& listings) {
        llvm::StringSet<> unique_dirs;
        ScopedTimer lookup;
        for(auto& group: graph.groups()) {
            for(auto& dir: toolchain.search_config(group).dirs) {
                unique_dirs.insert(dir.path);
            }
        }
        LOG_INFO("Config extracted: {} groups, {:.1f}ms", graph.groups().size(), lookup.ms_f());
        for(auto& entry: cdb.entries()) {
            unique_dirs.insert(files.spelling(entry.file).parent().str());
        }

        std::size_t listed = 0;
        for(auto& entry: unique_dirs) {
            if(files.dirs.kept(entry.getKey())) {
                continue;
            }
            listings.spawn(list_into(scanner.scope, entry.getKey().str()));
            listed += 1;
        }
        return listed;
    }

    static kota::task<> list_into(vfs::Scope& scope, std::string dir) {
        auto listing = co_await kota::queue([&dir] { return vfs::list(dir); });
        scope.adopt(dir, std::move(listing));
    }

    kota::task<> run(llvm::ArrayRef<CommandRef> units) {
        ScopedTimer timer;

        // One scan group per unique (command identity, input language) —
        // the SearchConfig granularity: different -I sets resolve
        // differently, and the same flags compiled as C and C++ pull
        // different implicit include sets. The identity is the command
        // hash, not the command: a command keeps the per-file flags the
        // compile drops (-o), which would give every unit its own group.
        llvm::MapVector<Fid, llvm::SmallVector<ScanContext, 1>> unit_contexts;
        llvm::DenseMap<std::pair<std::uint64_t, const char*>, std::uint32_t> group_ids;
        for(auto& unit: units) {
            auto [it, inserted] = group_ids.try_emplace(
                {cdb.commands().command_hash(unit.command), unit.input.value});
            if(inserted) {
                it->second = graph.add_group(unit);
            }
            auto& contexts = unit_contexts[unit.file];
            if(llvm::any_of(contexts,
                            [&](ScanContext known) { return known.group == it->second; })) {
                continue;
            }
            ScanContext context{.group = it->second, .unit = true};
            graph.add_context(unit.file, context);
            contexts.push_back(context);
        }

        // Pre-warm the toolchain cache: queries key by non-user-content
        // flags, so groups differing only in -D/-I collapse to the same
        // query — N groups often yield just 1-2 subprocess calls. They
        // run before the units' reads: query processes competing with the
        // reads for cores cost more than the overlap saves.
        toolchain.warm(graph.groups());

        // A wave's reads write into its scans until they join: it stays
        // put on the heap while the loop hands it on.
        auto wave = std::make_unique<Wave>();
        for(auto& [fid, contexts]: unit_contexts) {
            add(*wave, fid, contexts);
        }
        report.source_files = wave->files.size();

        kota::task_group<> listings;
        auto listed = list_directories(listings);
        co_await listings.join();
        LOG_INFO("Pre-populated dir cache: {} directories", listed);

        while(!wave->files.empty()) {
            ScopedTimer wave_timer;
            co_await wave->reads.join();
            auto waited = wave_timer.ms();

            auto next = std::make_unique<Wave>();
            auto reach = [&](Fid fid, ScanContext context) {
                add(*next, fid, llvm::ArrayRef(context));
            };
            for(auto [file, scanned]: llvm::zip(wave->files, wave->scans)) {
                report.total_files += 1;
                if(!scanned) {
                    LOG_WARN("Failed to read file for scanning: {}", files.resolve(file.fid));
                    continue;
                }
                if(!file.warm) {
                    files.disk.observe(file.fid, scanned->obs);
                    graph.quick_scans.results.try_emplace({file.fid, scanned->obs.hash},
                                                          scanned->result);
                }
                auto contexts = llvm::ArrayRef(wave->contexts).slice(file.first, file.count);
                for(auto& module: scanner.record(file.fid,
                                                 std::move(scanned->result),
                                                 scanned->obs.hash,
                                                 contexts,
                                                 reach)) {
                    graph.add_module(module.name, file.fid, module.internal);
                }
            }

            LOG_INFO("Wave {}: {} files | wait={}ms record={}ms | next={}",
                     report.waves,
                     wave->files.size(),
                     waited,
                     wave_timer.ms() - waited,
                     next->files.size());
            wave = std::move(next);
            report.waves += 1;
        }

        report.elapsed_ms = timer.ms();
        report.includes_resolved = scanner.includes_resolved;
        report.unresolved = std::move(scanner.unresolved);
    }

    CompilationDatabase& cdb;
    Toolchain& toolchain;
    DependencyGraph& graph;
    FileTable& files;
    ScanReport& report;
    FileScanner scanner;
    vfs::StatusBatch statuses;
};

}  // namespace

// Public sync entry point

ScanReport scan_dependency_graph(CompilationDatabase& cdb,
                                 Toolchain& toolchain,
                                 DependencyGraph& graph,
                                 llvm::ArrayRef<CommandRef> units) {
    ScanReport report;
    if(units.empty()) {
        return report;
    }

    FullScan scan(cdb, toolchain, graph, report);
    kota::run(scan.run(units));
    return report;
}

void rescan_dependency_graph(CompilationDatabase& cdb,
                             Toolchain& toolchain,
                             DependencyGraph& graph,
                             Fid path_id) {
    auto& files = cdb.files();
    FileScanner scanner(cdb, toolchain, graph);
    llvm::SmallVector<std::pair<Fid, ScanContext>> reached;
    auto reach = [&](Fid fid, ScanContext context) {
        reached.push_back({fid, context});
    };
    auto scan = [&](Fid fid, llvm::ArrayRef<ScanContext> contexts) {
        auto observed = vfs::read_observed(files.resolve(fid));
        if(!observed) {
            return;
        }
        files.disk.observe(fid, observed->obs);
        auto declared = scanner.record(
            fid,
            graph.quick_scans.scan_of(fid, observed->obs.hash, observed->content->getBuffer()),
            observed->obs.hash,
            contexts,
            reach);
        // A unit provides what its commands declare, as on the full scan;
        // a name it keeps declaring keeps its place among the providers.
        if(llvm::any_of(contexts, [](ScanContext context) { return context.unit; })) {
            graph.update_module_decl(fid, declared.empty() ? "" : declared.front().name);
            for(auto& module: declared) {
                graph.add_module(module.name, fid, module.internal);
            }
        }
    };

    graph.clear_includes(path_id);
    scan(path_id, llvm::to_vector(graph.contexts(path_id)));
    while(!reached.empty()) {
        auto [fid, context] = reached.pop_back_val();
        scan(fid, context);
    }
}

}  // namespace clice

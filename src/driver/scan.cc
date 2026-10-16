module;

#include "modules/prelude.h"

#include "support/logging.macros.h"

module clice;

import :config.config;
import :driver.driver;
import :project.configuration;
import :project.load;
import :project.project;
import :support.logging;
import :syntax.dependency_graph;
import :vfs.file_table;

namespace clice::driver {

namespace {

struct ScanOptions {
    kota::deco::decl::HelpOption help;

    DecoFlag(names = {"--json"},
             help =
                 "Print every scanned file with what it includes, the includes no search "
                 "found and the modules it provides",
             required = false)
    json;

    DecoKV(help =
               "Time this many scans, each from a fresh start (cold) and again over what "
               "it kept (warm, as after a database reload)",
           required = false)
    <std::uint32_t> runs;

    WorkspaceOptions where;
    LogLevelOption log;
};

struct ScannedFile {
    std::string path;

    /// A translation unit of the build, not a header the scan reached.
    bool source = false;

    std::vector<std::string> modules;
    std::vector<std::string> includes;

    /// As written: `<name>` or `"name"`.
    std::vector<std::string> unresolved;
};

struct ScanOutput {
    std::vector<ScannedFile> files;
};

/// A project loaded from scratch: the build's units and the scan from them.
struct Loaded {
    std::unique_ptr<FileTable> files = std::make_unique<FileTable>();
    std::unique_ptr<Project> project = std::make_unique<Project>(*files);
    BuildLoad load;
};

Loaded load(const Spelling& root, const Config& config, llvm::StringRef configuration) {
    Loaded loaded;
    loaded.files->spell_root(root);
    auto& project = *loaded.project;
    project.config = config;
    CanonicalPath identity(root);
    auto nearby = compile_commands_in_tree(project, identity, configuration);
    loaded.load = load_build(project, identity, configuration, nearby);
    return loaded;
}

ScanOutput describe(const Loaded& loaded) {
    auto& files = *loaded.files;
    auto& graph = loaded.project->dep_graph;

    llvm::DenseMap<Fid, std::vector<std::string>> modules;
    for(auto& entry: graph.modules()) {
        for(auto fid: entry.getValue()) {
            modules[fid].push_back(entry.getKey().str());
        }
    }
    llvm::DenseMap<Fid, std::vector<std::string>> unresolved;
    for(auto& include: loaded.load.scan.unresolved) {
        auto name = include.angled ? std::format("<{}>", include.name)
                                   : std::format("\"{}\"", include.name);
        auto& names = unresolved[include.includer];
        if(!llvm::is_contained(names, name)) {
            names.push_back(std::move(name));
        }
    }

    ScanOutput output;
    for(auto fid: graph.all_files()) {
        auto& file = output.files.emplace_back();
        file.path = files.display(fid);
        file.source =
            llvm::any_of(graph.contexts(fid), [](ScanContext context) { return context.unit; });
        if(auto it = modules.find(fid); it != modules.end()) {
            file.modules = std::move(it->second);
            std::ranges::sort(file.modules);
        }
        for(auto included: graph.get_all_includes(fid)) {
            file.includes.push_back(files.display(included));
        }
        if(auto it = unresolved.find(fid); it != unresolved.end()) {
            file.unresolved = std::move(it->second);
        }
    }
    std::ranges::sort(output.files, {}, &ScannedFile::path);
    return output;
}

void print_summary(const ScanOutput& output, const ScanReport& scan) {
    auto sources = static_cast<std::size_t>(
        llvm::count_if(output.files, [](const ScannedFile& file) { return file.source; }));
    auto headers = output.files.size() - sources;
    auto found = scan.includes_resolved + scan.unresolved.size();
    driver::println(
        "Scanned {} file{} ({} source{}, {} header{}) in {}ms: {} of {} include{} resolved.",
        output.files.size(),
        plural_s(output.files.size()),
        sources,
        plural_s(sources),
        headers,
        plural_s(headers),
        scan.elapsed_ms,
        scan.includes_resolved,
        found,
        plural_s(found));
    for(auto& file: output.files) {
        for(auto& name: file.unresolved) {
            driver::println("  {}: {} not found", file.path, name);
        }
    }
}

/// The smallest and the median of `times`, which it sorts; an even count
/// takes the lower middle, as the benchmarks' statistics do.
std::pair<std::int64_t, std::int64_t> spread(std::vector<std::int64_t>& times) {
    std::ranges::sort(times);
    return {times.front(), times[(times.size() - 1) / 2]};
}

/// Scan `runs` times, each cold from a fresh load and warm over what it
/// kept, and print the times.
void time_scans(Loaded first,
                std::uint32_t runs,
                const Spelling& root,
                const Config& config,
                llvm::StringRef configuration) {
    std::vector<std::int64_t> cold;
    std::vector<std::int64_t> warm;
    for(std::uint32_t run = 0; run < runs; run += 1) {
        auto loaded = run == 0 ? std::move(first) : load(root, config, configuration);
        auto& project = *loaded.project;
        project.dep_graph.reset();
        auto rescan = scan_dependency_graph(project.cdb,
                                            project.dep_graph,
                                            project.build.units(loaded.load.members));
        cold.push_back(loaded.load.scan.elapsed_ms);
        warm.push_back(rescan.elapsed_ms);
        driver::println("Run {}: cold {}ms, warm {}ms", run + 1, cold.back(), warm.back());
    }
    auto [cold_min, cold_median] = spread(cold);
    auto [warm_min, warm_median] = spread(warm);
    driver::println("Cold: min {}ms, median {}ms. Warm: min {}ms, median {}ms.",
                    cold_min,
                    cold_median,
                    warm_min,
                    warm_median);
}

int run_scan(const ScanOptions& opts) {
    if(opts.runs && static_cast<bool>(opts.json)) {
        LOG_ERROR("--runs prints times, --json the graph: pass one of them");
        return 1;
    }
    if(opts.runs && *opts.runs == 0) {
        LOG_ERROR("--runs takes a count of at least 1");
        return 1;
    }
    auto root = opts.where.root();
    if(!workspace_exists(root)) {
        return 1;
    }
    auto config = Config::load_from_workspace(CanonicalPath(root));
    auto requested = opts.where.configuration.tag();
    if(auto checked = check_requested_configuration(config, requested); !checked) {
        LOG_ERROR("{}", checked.error());
        return 1;
    }
    auto configuration = resolve_configuration(config, requested);

    auto first = load(root, config, configuration);
    if(first.load.members.empty()) {
        LOG_ERROR("Nothing to scan: no compilation database for {} lists a file", root.str());
        return 1;
    }
    auto output = describe(first);
    if(static_cast<bool>(opts.json)) {
        print_json(output);
        return 0;
    }
    print_summary(output, first.load.scan);
    if(opts.runs) {
        time_scans(std::move(first), *opts.runs, root, config, configuration);
    }
    return 0;
}

CommandHandler build(const char*) {
    return operation<ScanOptions>("clice scan [OPTIONS]", "scan", Errors::Text, run_scan);
}

[[maybe_unused]] const bool registered = register_command(
    Command::scan,
    {.description = "Scan the project's include and module dependencies", .build = build});

}  // namespace

}  // namespace clice::driver

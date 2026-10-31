module clice;

import :command.file_kind;
import :index.project_index;
import :project.build;
import :project.project;
import :project.queries;
import :syntax.dependency_graph;
import :vfs.file_table;
import :vfs.path;

namespace clice {

/// The files reachable from `root` along `adjacent`, breadth first and
/// each once: direct neighbours at depth 1, `max_depth` levels at most,
/// 0 meaning unbounded.
static std::vector<DepEntry>
    collect_deps(Project& project,
                 Fid root,
                 int max_depth,
                 llvm::function_ref<llvm::SmallVector<Fid>(Fid)> adjacent) {
    std::vector<DepEntry> entries;
    llvm::SmallVector<std::pair<Fid, int>> queue{
        {root, 0}
    };
    llvm::DenseSet<Fid> visited{root};
    for(std::size_t i = 0; i < queue.size(); i += 1) {
        auto [id, depth] = queue[i];
        if(max_depth > 0 && depth >= max_depth) {
            continue;
        }
        for(auto next: adjacent(id)) {
            if(!visited.insert(next).second) {
                continue;
            }
            queue.push_back({next, depth + 1});
            entries.push_back({.path = project.file_table.display(next), .depth = depth + 1});
        }
    }
    return entries;
}

std::expected<ProjectFilesResult, std::string> Project::project_files(llvm::StringRef filter) {
    if(!ranges::contains(std::array<llvm::StringRef, 4>{"all", "source", "header", "module"},
                         filter)) {
        return std::unexpected(
            std::format("invalid filter '{}': expected all, source, header or module",
                        std::string_view(filter)));
    }
    ProjectFilesResult result;
    llvm::DenseSet<Fid> seen;
    for(auto member: build.members()) {
        auto file_path = file_table.resolve(member);
        if(file_path.empty() || !seen.insert(member).second) {
            continue;
        }
        auto module_name = dep_graph.module_of(member);
        llvm::StringRef kind = !module_name.empty()        ? "module"
                               : is_header_path(file_path) ? "header"
                                                           : "source";
        if(filter != "all" && filter != kind) {
            continue;
        }
        FileInfo info{.path = file_table.display(member), .kind = kind.str()};
        if(!module_name.empty()) {
            info.module_name = module_name.str();
        }
        result.files.push_back(std::move(info));
    }
    if(filter == "all" || filter == "header") {
        index.for_each_indexed_file([&](Fid path_id) {
            auto path = file_table.resolve(path_id);
            if(!seen.contains(path_id) && is_header_path(path)) {
                seen.insert(path_id);
                result.files.push_back({.path = file_table.display(path_id), .kind = "header"});
            }
        });
    }
    result.total = static_cast<int>(result.files.size());
    return result;
}

std::expected<FileDepsResult, std::string> Project::file_deps(const Spelling& path,
                                                              llvm::StringRef direction,
                                                              int depth) {
    if(!ranges::contains(std::array<llvm::StringRef, 3>{"includes", "includers", "both"},
                         direction)) {
        return std::unexpected(
            std::format("invalid direction '{}': expected includes, includers or both",
                        std::string_view(direction)));
    }
    if(depth < 0) {
        return std::unexpected("depth must not be negative");
    }
    if(!fs::is_file(path)) {
        return std::unexpected(std::format("no such file: {}", path));
    }
    FileDepsResult result{.file = path.str()};
    auto file = file_table.find(path);
    if(!file) {
        return result;
    }
    result.file = file_table.display(*file);
    if(direction != "includers") {
        result.includes = collect_deps(*this, *file, depth, [&](Fid id) {
            return dep_graph.get_all_includes(id);
        });
    }
    if(direction != "includes") {
        result.includers = collect_deps(*this, *file, depth, [&](Fid id) {
            return llvm::SmallVector<Fid>(dep_graph.get_includers(id));
        });
    }
    return result;
}

std::expected<ImpactAnalysisResult, std::string> Project::impact_analysis(const Spelling& path) {
    if(!fs::is_file(path)) {
        return std::unexpected(std::format("no such file: {}", path));
    }
    ImpactAnalysisResult result;
    auto file = file_table.find(path);
    if(!file) {
        return result;
    }
    llvm::DenseSet<Fid> seen{*file};
    for(auto dependent: llvm::concat<const Fid>(dep_graph.get_includers(*file),
                                                dep_graph.get_forcing_units(*file))) {
        if(seen.insert(dependent).second) {
            result.direct_dependents.push_back(file_table.display(dependent));
        }
    }
    auto readers = dep_graph.find_readers(*file);
    // An importer reads the files its modules were built from without
    // including them.
    llvm::SmallVector<Fid> importers;
    index.for_each_importer(*file, [&](Fid importer) { importers.push_back(importer); });
    ranges::sort(importers, [&](Fid lhs, Fid rhs) {
        return file_table.resolve(lhs) < file_table.resolve(rhs);
    });
    for(auto importer: importers) {
        if(!ranges::contains(readers, importer)) {
            readers.push_back(importer);
        }
    }
    for(auto reader: readers) {
        if(seen.insert(reader).second) {
            result.transitive_dependents.push_back(file_table.display(reader));
        }
    }
    for(auto reader: readers) {
        auto module_name = dep_graph.module_of(reader);
        if(!module_name.empty()) {
            result.affected_modules.push_back(module_name.str());
        }
    }
    auto module_name = dep_graph.module_of(*file);
    if(!module_name.empty()) {
        result.affected_modules.push_back(module_name.str());
    }
    return result;
}

bool Project::units_pending() {
    return ranges::any_of(build.members(), [&](Fid unit) {
        return build.indexed(file_table.resolve(unit)) && !index.indexed(unit);
    });
}

}  // namespace clice

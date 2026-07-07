#include "index/include_graph.h"

#include "compile/compilation_unit.h"

namespace clice::index {

static std::uint32_t addIncludeChain(CompilationUnitRef unit,
                                     clang::FileID fid,
                                     IncludeGraph& graph,
                                     llvm::StringMap<std::uint32_t>& path_table) {
    auto include_loc = unit.include_location(fid);
    if(include_loc.isInvalid()) {
        return -1;
    }

    auto& [paths, path_hashes, locations, file_table] = graph;

    auto [iter, success] = file_table.try_emplace(fid, locations.size());
    if(!success) {
        return iter->second;
    }

    auto index = iter->second;

    {
        auto presumed = unit.presumed_location(include_loc);
        locations.emplace_back();
        locations[index].line = presumed.getLine();

        auto path = unit.file_path(fid);
        auto [iter, success] = path_table.try_emplace(path, paths.size());
        if(success) {
            paths.emplace_back(path);
        }
        locations[index].path_id = iter->second;

        uint32_t include = -1;
        if(presumed.getIncludeLoc().isValid()) {
            include =
                addIncludeChain(unit, unit.file_id(presumed.getIncludeLoc()), graph, path_table);
        }
        locations[index].include = include;
    }

    return index;
}

IncludeGraph IncludeGraph::from(CompilationUnitRef unit) {
    llvm::StringMap<std::uint32_t> path_table;
    IncludeGraph graph;
    for(auto fid: unit.files()) {
        graph.file_table[fid] = addIncludeChain(unit, fid, graph, path_table);
    }
    graph.paths.emplace_back(unit.file_path(unit.interested_file()));

    // Hash every path's resident buffer — the content this compilation
    // actually consumed, immune to concurrent disk writes.
    graph.path_hashes.assign(graph.paths.size(), 0);
    for(auto [fid, location]: graph.file_table) {
        if(location != std::uint32_t(-1)) {
            graph.path_hashes[graph.locations[location].path_id] =
                llvm::xxh3_64bits(unit.file_content(fid));
        }
    }
    graph.path_hashes.back() = llvm::xxh3_64bits(unit.file_content(unit.interested_file()));

    return graph;
}

}  // namespace clice::index

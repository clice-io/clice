#include "index/include_graph.h"

#include "compile/compilation_unit.h"
#include "support/logging.h"

#include "llvm/Support/xxhash.h"

namespace clice::index {

static std::uint32_t addIncludeChain(CompilationUnitRef unit,
                                     clang::FileID fid,
                                     IncludeGraph& graph,
                                     llvm::StringMap<std::uint32_t>& path_table) {
    auto include_loc = unit.include_location(fid);
    if(include_loc.isInvalid()) {
        return -1;
    }

    auto& [paths, locations, path_hashes, file_table] = graph;

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

IncludeGraph IncludeGraph::from(CompilationUnitRef unit,
                                llvm::ArrayRef<clang::FileID> indexed_fids) {
    llvm::StringMap<std::uint32_t> path_table;
    IncludeGraph graph;

    for(auto& [fid, directive]: unit.directives()) {
        for(auto& include: directive.includes) {
            if(!include.skipped && include.fid.isValid()) {
                graph.file_table[include.fid] =
                    addIncludeChain(unit, include.fid, graph, path_table);
            }
        }
    }

    for(auto fid: indexed_fids) {
        graph.file_table[fid] = addIncludeChain(unit, fid, graph, path_table);
    }

    auto interested = unit.interested_file();
    graph.file_table[interested] = addIncludeChain(unit, interested, graph, path_table);
    graph.paths.emplace_back(unit.file_path(interested));

    // Hash the consumed bytes per path from the compiler's own buffers.
    // Freshness checks compare the disk against these, so they must
    // describe what the rows were built from — a fid whose buffer never
    // loaded here (preamble header behind a PCH) contributes no hash and
    // its consumers stay conservative.
    graph.path_hashes.assign(graph.paths.size(), 0);
    auto hash_fid = [&](clang::FileID fid, std::uint32_t path_id) {
        if(graph.path_hashes[path_id] != 0) {
            return;
        }
        if(auto content = unit.loaded_file_content(fid)) {
            graph.path_hashes[path_id] = llvm::xxh3_64bits(*content);
        }
    };
    for(auto& [fid, location]: graph.file_table) {
        if(location != static_cast<std::uint32_t>(-1)) {
            hash_fid(fid, graph.locations[location].path_id);
        }
    }
    hash_fid(interested, graph.paths.size() - 1);
    return graph;
}

std::uint32_t IncludeGraph::include_location_id(clang::FileID fid) const {
    auto it = file_table.find(fid);
    if(it == file_table.end()) [[unlikely]] {
        LOG_WARN("IncludeGraph: fid {} missing from file table, attributing to interested file",
                 fid.getHashValue());
        return -1;
    }
    return it->second;
}

}  // namespace clice::index

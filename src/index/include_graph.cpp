#include "index/include_graph.h"

#include "compile/compilation_unit.h"
#include "support/logging.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
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

        // The location of the file CONTAINING the directive — the parent
        // consumers pair `line` with. Recursing on the directive's own fid
        // (not the presumed include loc, which names the containing
        // file's includer and sat one level off) bottoms out at -1 for
        // directives written in the main file.
        auto include = addIncludeChain(unit, unit.file_id(include_loc), graph, path_table);
        locations[index].include = include;
    }

    return index;
}

IncludeGraph IncludeGraph::from(CompilationUnitRef unit,
                                llvm::ArrayRef<clang::FileID> indexed_fids) {
    llvm::StringMap<std::uint32_t> path_table;
    IncludeGraph graph;

    // Path and location ids are assigned in first-visit order and the
    // envelope's byte hash is an identity, so the visit order must be a
    // pure function of the parse — sort every fid set that arrives in
    // DenseMap iteration order.
    auto& directives = unit.directives();
    llvm::SmallVector<clang::FileID> directive_fids;
    directive_fids.reserve(directives.size());
    for(auto fid: llvm::make_first_range(directives)) {
        directive_fids.push_back(fid);
    }
    llvm::sort(directive_fids);
    for(auto fid: directive_fids) {
        for(auto& include: directives.find(fid)->second.includes) {
            if(!include.skipped && include.fid.isValid()) {
                graph.file_table[include.fid] =
                    addIncludeChain(unit, include.fid, graph, path_table);
            }
        }
    }

    llvm::SmallVector<clang::FileID> sorted_indexed(indexed_fids.begin(), indexed_fids.end());
    llvm::sort(sorted_indexed);
    for(auto fid: sorted_indexed) {
        graph.file_table[fid] = addIncludeChain(unit, fid, graph, path_table);
    }

    auto main_fid = unit.main_file();
    graph.file_table[main_fid] = addIncludeChain(unit, main_fid, graph, path_table);
    graph.paths.emplace_back(unit.file_path(main_fid));

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
    hash_fid(main_fid, graph.paths.size() - 1);
    return graph;
}

std::uint32_t IncludeGraph::include_location_id(clang::FileID fid) const {
    auto it = file_table.find(fid);
    if(it == file_table.end()) [[unlikely]] {
        LOG_WARN("IncludeGraph: fid {} missing from file table, attributing to main file",
                 fid.getHashValue());
        return -1;
    }
    return it->second;
}

}  // namespace clice::index

#include "sched/hosting.h"

#include <algorithm>
#include <tuple>

#include "sched/workspace.h"
#include "support/filesystem.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/Path.h"
#include "clang/Driver/Types.h"

namespace clice {

namespace {

/// The language family a file belongs to by its suffix, or `Any` when
/// the suffix does not say (`.h`, an unknown extension).
enum class Family : std::uint8_t {
    Any,
    C,
    CXX,
    CUDA,
    Other,
};

clang::driver::types::ID type_of_suffix(llvm::StringRef path) {
    namespace types = clang::driver::types;
    auto ext = path::extension(path);
    ext.consume_front(".");
    return ext.empty() ? types::TY_INVALID : types::lookupTypeForExtension(ext);
}

/// CUDA's header convention is missing from clang's extension table.
bool header_suffix(llvm::StringRef path) {
    namespace types = clang::driver::types;
    auto type = type_of_suffix(path);
    return path::extension(path) == ".cuh" ||
           (type != types::TY_INVALID && types::onlyPrecompileType(type));
}

Family family_of_suffix(llvm::StringRef path) {
    namespace types = clang::driver::types;
    if(path::extension(path) == ".cuh") {
        return Family::CUDA;
    }
    auto type = type_of_suffix(path);
    if(type == types::TY_INVALID || type == types::TY_CHeader) {
        return Family::Any;
    }
    if(types::isCuda(type)) {
        return Family::CUDA;
    }
    if(types::isCXX(type)) {
        return Family::CXX;
    }
    return types::isDerivedFromC(type) ? Family::C : Family::Other;
}

/// The family of a unit by the clang language its command compiles it as.
Family family_of_unit(Workspace& workspace, Fid unit) {
    auto commands = workspace.build.commands(unit);
    llvm::StringRef language =
        workspace.cdb.input_kind(commands.front().config, workspace.file_table.resolve(unit)).value;
    if(language.contains("cuda")) {
        return Family::CUDA;
    }
    if(language.contains("c++")) {
        return Family::CXX;
    }
    if(language.starts_with("c") || language.starts_with("objective-c")) {
        return Family::C;
    }
    return Family::Other;
}

bool compatible(Family file, Family unit) {
    return file == Family::Any || file == unit;
}

/// The units whose header search directories contain `dir`, nearest
/// directory first.
llvm::SmallVector<Fid> units_searching(Workspace& workspace, llvm::StringRef dir) {
    auto& index = workspace.search_dir_units;
    if(workspace.search_dir_units_epoch != workspace.context_epoch) {
        index.clear();
        for(auto& unit: workspace.build.units(workspace.build.members())) {
            for(auto& search_dir: workspace.cdb.search_config(unit).dirs) {
                auto canonical = search_dir.path;
                path::canonicalize(canonical);
                auto& bucket = index[canonical];
                if(!llvm::is_contained(bucket, unit.file)) {
                    bucket.push_back(unit.file);
                }
            }
        }
        for(auto& bucket: index) {
            std::ranges::sort(bucket.getValue(), [&](Fid a, Fid b) {
                return workspace.file_table.resolve(a) < workspace.file_table.resolve(b);
            });
        }
        workspace.search_dir_units_epoch = workspace.context_epoch;
    }
    llvm::SmallVector<Fid> units;
    path::walk_ancestors(dir, "", [&](llvm::StringRef ancestor) {
        if(auto it = index.find(ancestor); it != index.end()) {
            units.append(it->second);
        }
        return true;
    });
    return units;
}

}  // namespace

std::optional<Fid> command_donor(Workspace& workspace, Fid file) {
    auto& files = workspace.file_table;
    auto path = files.resolve(file);
    auto family = family_of_suffix(path);
    auto dir = path::parent_path(path);
    auto stem = path::stem(path);

    llvm::SmallVector<Fid> units;
    for(auto member: workspace.build.members()) {
        if(member != file && compatible(family, family_of_unit(workspace, member))) {
            units.push_back(member);
        }
    }
    if(units.empty()) {
        return std::nullopt;
    }

    auto siblings = llvm::to_vector(llvm::make_filter_range(units, [&](Fid unit) {
        return path::parent_path(files.resolve(unit)) == dir;
    }));
    if(!siblings.empty()) {
        auto key = [&](Fid unit) {
            auto unit_path = files.resolve(unit);
            return std::tuple(path::stem(unit_path) != stem, unit_path);
        };
        return *std::ranges::min_element(siblings, {}, key);
    }

    // A header some unit's header search reaches: that unit's code finds
    // it by that path, so its command is the one the header is written for.
    if(header_suffix(path)) {
        for(auto unit: units_searching(workspace, dir)) {
            if(llvm::is_contained(units, unit)) {
                return unit;
            }
        }
    }

    // The closest unit by path: the longest shared prefix, then by name.
    auto shared_prefix = [&](Fid unit) {
        auto unit_path = files.resolve(unit);
        std::size_t common = 0;
        auto n = std::min(unit_path.size(), path.size());
        while(common < n && unit_path[common] == path[common]) {
            common += 1;
        }
        return common;
    };
    return *std::ranges::min_element(units, {}, [&](Fid unit) {
        return std::tuple(path.size() - shared_prefix(unit), files.resolve(unit));
    });
}

llvm::SmallVector<Fid> ranked_hosts(Workspace& workspace, Fid header) {
    auto& files = workspace.file_table;
    auto header_path = files.resolve(header);
    auto header_stem = llvm::sys::path::stem(header_path);
    auto header_dir = llvm::sys::path::parent_path(header_path);
    auto sources = workspace.build.source_order(header_path);
    auto family = family_of_suffix(header_path);

    llvm::SmallVector<Fid> hosts;
    for(auto candidate: workspace.dep_graph.find_host_sources(header)) {
        if(!workspace.build.commands(candidate).empty() &&
           compatible(family, family_of_unit(workspace, candidate))) {
            hosts.push_back(candidate);
        }
    }

    auto score = [&](Fid host) -> std::tuple<std::size_t, int, int, std::size_t> {
        auto host_path = files.resolve(host);
        // A host compiled from a database the header's rules name comes
        // first; one living on a default command comes after every
        // database.
        std::size_t source_rank = sources.size();
        if(auto entries = workspace.build.entries(host); !entries.empty()) {
            source_rank = llvm::find(sources, entries.front().source) - sources.begin();
        }
        int stem_match = llvm::sys::path::stem(host_path) == header_stem ? 0 : 1;
        int same_dir = llvm::sys::path::parent_path(host_path) == header_dir ? 0 : 1;
        // Longer shared prefix means "closer" in the tree; measured against
        // the header's own length so every candidate shares one baseline.
        std::size_t common = 0;
        auto n = std::min(host_path.size(), header_path.size());
        while(common < n && host_path[common] == header_path[common]) {
            common += 1;
        }
        return {source_rank, stem_match, same_dir, header_path.size() - common};
    };
    std::ranges::sort(hosts, [&](Fid a, Fid b) {
        auto sa = score(a), sb = score(b);
        if(sa != sb) {
            return sa < sb;
        }
        return files.resolve(a) < files.resolve(b);
    });
    return hosts;
}

std::optional<Host> default_host(Workspace& workspace, Fid header) {
    for(auto host: ranked_hosts(workspace, header)) {
        auto chain = workspace.dep_graph.find_include_chain(host, header);
        if(!chain.empty()) {
            return Host{.file = host, .chain = std::move(chain)};
        }
    }
    return std::nullopt;
}

}  // namespace clice

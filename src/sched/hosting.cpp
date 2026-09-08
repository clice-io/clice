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

namespace types = clang::driver::types;

/// Whether the suffix names a header — or nothing clang knows, which a
/// file under a header search directory usually is (`.inc`, `.ipp`).
bool header_suffix(llvm::StringRef path) {
    auto type = suffix_type(path);
    return type == types::TY_INVALID || types::onlyPrecompileType(type);
}

/// The language a command compiles its unit as — a `-x` in the entry or
/// a rule's append included, else the unit's suffix.
types::ID language_of(const CommandRef& command) {
    return types::lookupTypeForTypeSpecifier(command.input.value);
}

CommandRef effective(Workspace& workspace, Fid unit, const Candidate& command) {
    auto path = workspace.file_table.resolve(unit);
    return workspace.build.resolve(unit, command.config, command.source, path, path);
}

/// Whether the file at `path` can be part of a translation unit compiled
/// as `language`. A source only in its own: rendering the borrowed
/// command for it would otherwise force `-x`, and a `.cpp` compiled as
/// CUDA or a `.m` as C is not the file. A header has latitude: a `.h`
/// fits any, a C++ header every language built on C++ (Objective-C++,
/// CUDA, HIP), a `.cuh` CUDA.
bool compatible(llvm::StringRef path, types::ID language) {
    auto file = suffix_type(path);
    if(file == types::TY_INVALID) {
        return path::extension(path) != ".cuh" || types::isCuda(language) || types::isHIP(language);
    }
    if(file == types::TY_CHeader) {
        return true;
    }
    if(types::onlyPrecompileType(file) && types::isCXX(file)) {
        return types::isCXX(language);
    }
    return file == language;
}

std::size_t shared_prefix(llvm::StringRef a, llvm::StringRef b) {
    std::size_t common = 0;
    auto n = std::min(a.size(), b.size());
    while(common < n && a[common] == b[common]) {
        common += 1;
    }
    return common;
}

const LenderIndex& lender_index(Workspace& workspace) {
    auto& index = workspace.lenders;
    if(index.epoch == workspace.commands_epoch) {
        return index;
    }
    index.commands.clear();
    index.search_dirs.clear();
    auto members = workspace.build.members();
    std::ranges::sort(members, {}, [&](Fid unit) { return workspace.file_table.resolve(unit); });
    for(auto member: members) {
        // A member a rule claims with a default command that is no compile
        // command has none.
        for(auto& command: workspace.build.commands(member)) {
            auto ref = effective(workspace, member, command);
            auto position = static_cast<std::uint32_t>(index.commands.size());
            index.commands.push_back({
                .lender = {.unit = member, .config = command.config},
                .language = language_of(ref),
            });
            for(auto& search_dir: workspace.cdb.search_config(ref).dirs) {
                auto canonical = search_dir.path;
                path::canonicalize(canonical);
                index.search_dirs[canonical].push_back(position);
            }
        }
    }
    index.epoch = workspace.commands_epoch;
    return index;
}

}  // namespace

std::optional<Lender> command_lender(Workspace& workspace, Fid file) {
    auto& files = workspace.file_table;
    auto path = files.resolve(file);
    bool header = header_suffix(path);
    auto dir = path::parent_path(path);
    auto stem = path::stem(path);
    auto& index = lender_index(workspace);
    auto fits = [&](const LenderIndex::Command& command) {
        return compatible(path, command.language);
    };

    // Every unit with its first command of the family, in path order.
    llvm::SmallVector<Lender> units;
    for(auto& command: index.commands) {
        if(fits(command) && (units.empty() || units.back().unit != command.lender.unit)) {
            units.push_back(command.lender);
        }
    }
    if(units.empty()) {
        return std::nullopt;
    }
    auto unit_path = [&](const Lender& lender) {
        return files.resolve(lender.unit);
    };

    auto siblings = llvm::to_vector(llvm::make_filter_range(units, [&](const Lender& lender) {
        return path::parent_path(unit_path(lender)) == dir;
    }));
    if(!siblings.empty()) {
        return *std::ranges::min_element(siblings, {}, [&](const Lender& lender) {
            return std::tuple(path::stem(unit_path(lender)) != stem, unit_path(lender));
        });
    }

    // A header some command's header search reaches: that unit's code
    // finds it by that path, so the command is the one the header is
    // written for. Nearest directory first, by path and command within.
    if(header) {
        std::optional<Lender> found;
        path::walk_ancestors(dir, "", [&](llvm::StringRef ancestor) {
            if(auto it = index.search_dirs.find(ancestor); it != index.search_dirs.end()) {
                for(auto position: it->second) {
                    if(fits(index.commands[position])) {
                        found = index.commands[position].lender;
                        return false;
                    }
                }
            }
            return true;
        });
        if(found) {
            return found;
        }
    }

    // The closest unit by path: the longest shared prefix, then by name.
    return *std::ranges::min_element(units, {}, [&](const Lender& lender) {
        return std::tuple(path.size() - shared_prefix(unit_path(lender), path), unit_path(lender));
    });
}

llvm::SmallVector<Candidate, 2> host_commands(Workspace& workspace, Fid header, Fid host) {
    auto header_path = workspace.file_table.resolve(header);
    llvm::SmallVector<Candidate, 2> fitting;
    for(auto& command: workspace.build.commands(host)) {
        if(compatible(header_path, language_of(effective(workspace, host, command)))) {
            fitting.push_back(command);
        }
    }
    return fitting;
}

llvm::SmallVector<Fid> ranked_hosts(Workspace& workspace, Fid header) {
    auto& files = workspace.file_table;
    auto header_path = files.resolve(header);
    auto header_stem = llvm::sys::path::stem(header_path);
    auto header_dir = llvm::sys::path::parent_path(header_path);
    auto sources = workspace.build.source_order(header_path);

    llvm::SmallVector<Fid> hosts;
    for(auto candidate: workspace.dep_graph.find_host_sources(header)) {
        if(!host_commands(workspace, header, candidate).empty()) {
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
        return {source_rank,
                stem_match,
                same_dir,
                header_path.size() - shared_prefix(host_path, header_path)};
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

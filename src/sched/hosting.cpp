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

/// Whether the suffix names a header — or nothing clang knows, which a
/// file under a header search directory usually is (`.inc`, `.ipp`).
bool header_suffix(llvm::StringRef path) {
    namespace types = clang::driver::types;
    auto type = suffix_type(path);
    return type == types::TY_INVALID || types::onlyPrecompileType(type);
}

Family family_of_suffix(llvm::StringRef path) {
    namespace types = clang::driver::types;
    if(path::extension(path) == ".cuh") {
        return Family::CUDA;
    }
    auto type = suffix_type(path);
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

/// The family of a file by the language its effective command compiles
/// it as — a `-x` in the entry or a rule's append included.
Family family_of_command(const CommandRef& command) {
    llvm::StringRef language = command.input.value;
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

CommandRef effective(Workspace& workspace, Fid unit, const Candidate& command) {
    auto path = workspace.file_table.resolve(unit);
    return workspace.build.resolve(unit, command.config, command.source, path, path);
}

/// The first of the unit's commands compiling it in the file's family —
/// any when the file's suffix does not say — or none.
/// Whether a file of `family` can be part of a command's translation
/// unit. A CUDA unit is C++ with device code, so a C++ header fits it (a
/// `.cuh` needs CUDA itself); a C++ source borrowing a CUDA command would
/// compile as CUDA, so only headers get that latitude.
bool compatible(Family file, Family command, bool header) {
    return file == Family::Any || file == command ||
           (header && file == Family::CXX && command == Family::CUDA);
}

const Candidate* compatible_command(Workspace& workspace,
                                    Family family,
                                    bool header,
                                    Fid unit,
                                    llvm::ArrayRef<Candidate> commands) {
    if(family == Family::Any) {
        return commands.empty() ? nullptr : &commands.front();
    }
    auto it = llvm::find_if(commands, [&](const Candidate& command) {
        return compatible(family, family_of_command(effective(workspace, unit, command)), header);
    });
    return it == commands.end() ? nullptr : &*it;
}

std::size_t shared_prefix(llvm::StringRef a, llvm::StringRef b) {
    std::size_t common = 0;
    auto n = std::min(a.size(), b.size());
    while(common < n && a[common] == b[common]) {
        common += 1;
    }
    return common;
}

/// The lenders whose command's header search directories contain `dir`,
/// nearest directory first, by path and command within one.
llvm::SmallVector<Lender> lenders_searching(Workspace& workspace, llvm::StringRef dir) {
    auto& index = workspace.search_dir_lenders;
    if(workspace.search_dir_lenders_epoch != workspace.commands_epoch) {
        index.clear();
        for(auto member: workspace.build.members()) {
            for(auto& command: workspace.build.commands(member)) {
                auto ref = effective(workspace, member, command);
                for(auto& search_dir: workspace.cdb.search_config(ref).dirs) {
                    auto canonical = search_dir.path;
                    path::canonicalize(canonical);
                    auto& bucket = index[canonical];
                    if(!llvm::is_contained(bucket, std::pair(member, command.config))) {
                        bucket.emplace_back(member, command.config);
                    }
                }
            }
        }
        for(auto& bucket: index) {
            std::ranges::sort(bucket.getValue(), {}, [&](const std::pair<Fid, ConfigID>& lender) {
                return std::tuple(workspace.file_table.resolve(lender.first), lender.second);
            });
        }
        workspace.search_dir_lenders_epoch = workspace.commands_epoch;
    }
    llvm::SmallVector<Lender> lenders;
    path::walk_ancestors(dir, "", [&](llvm::StringRef ancestor) {
        if(auto it = index.find(ancestor); it != index.end()) {
            for(auto& [unit, config]: it->second) {
                lenders.push_back({.unit = unit, .config = config});
            }
        }
        return true;
    });
    return lenders;
}

}  // namespace

std::optional<Lender> command_lender(Workspace& workspace, Fid file) {
    auto& files = workspace.file_table;
    auto path = files.resolve(file);
    auto family = family_of_suffix(path);
    bool header = header_suffix(path);
    auto dir = path::parent_path(path);
    auto stem = path::stem(path);

    // Every unit with a command of the family; a member a rule claims with
    // a default command that is no compile command has none.
    llvm::SmallVector<Lender> units;
    for(auto member: workspace.build.members()) {
        auto commands = workspace.build.commands(member);
        if(auto* command = compatible_command(workspace, family, header, member, commands)) {
            units.push_back({.unit = member, .config = command->config});
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
    // written for.
    if(header) {
        for(auto& lender: lenders_searching(workspace, dir)) {
            auto commands = workspace.build.commands(lender.unit);
            auto command = llvm::find_if(commands, [&](const Candidate& candidate) {
                return candidate.config == lender.config;
            });
            if(command != commands.end() && compatible_command(workspace,
                                                               family,
                                                               header,
                                                               lender.unit,
                                                               llvm::ArrayRef(*command))) {
                return lender;
            }
        }
    }

    // The closest unit by path: the longest shared prefix, then by name.
    return *std::ranges::min_element(units, {}, [&](const Lender& lender) {
        return std::tuple(path.size() - shared_prefix(unit_path(lender), path), unit_path(lender));
    });
}

llvm::SmallVector<Fid> ranked_hosts(Workspace& workspace, Fid header) {
    auto& files = workspace.file_table;
    auto header_path = files.resolve(header);
    auto header_stem = llvm::sys::path::stem(header_path);
    auto header_dir = llvm::sys::path::parent_path(header_path);
    auto sources = workspace.build.source_order(header_path);
    auto family = family_of_suffix(header_path);

    // A host lends its first command (see pick_pinned_config), so that is
    // the one whose language must fit.
    llvm::SmallVector<Fid> hosts;
    for(auto candidate: workspace.dep_graph.find_host_sources(header)) {
        auto commands = workspace.build.commands(candidate);
        if(compatible_command(workspace,
                              family,
                              /*header=*/true,
                              candidate,
                              llvm::ArrayRef(commands).take_front(1))) {
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

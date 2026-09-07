#include "sched/hosting.h"

#include <algorithm>
#include <tuple>

#include "sched/workspace.h"
#include "support/filesystem.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "clang/Driver/Types.h"

namespace clice {

namespace {

/// Whether the suffix names a header — or nothing clang knows, which a
/// file under a header search directory usually is (`.inc`, `.ipp`).
bool header_suffix(llvm::StringRef path) {
    namespace types = clang::driver::types;
    auto type = suffix_type(path);
    return type == types::TY_INVALID || types::onlyPrecompileType(type);
}

Language family_of_suffix(llvm::StringRef path) {
    namespace types = clang::driver::types;
    if(path::extension(path) == ".cuh") {
        return Language::CUDA;
    }
    auto type = suffix_type(path);
    if(type == types::TY_INVALID || type == types::TY_CHeader) {
        return Language::Any;
    }
    if(types::isCuda(type)) {
        return Language::CUDA;
    }
    if(types::isObjC(type)) {
        return types::isCXX(type) ? Language::ObjCXX : Language::ObjC;
    }
    if(types::isCXX(type)) {
        return Language::CXX;
    }
    return types::isDerivedFromC(type) ? Language::C : Language::Other;
}

/// The family of a file by the language its effective command compiles
/// it as — a `-x` in the entry or a rule's append included.
Language family_of_command(const CommandRef& command) {
    llvm::StringRef language = command.input.value;
    if(language.contains("cuda")) {
        return Language::CUDA;
    }
    if(language.starts_with("objective-c")) {
        return language.contains("c++") ? Language::ObjCXX : Language::ObjC;
    }
    if(language.contains("c++")) {
        return Language::CXX;
    }
    if(language.starts_with("c")) {
        return Language::C;
    }
    return Language::Other;
}

CommandRef effective(Workspace& workspace, Fid unit, const Candidate& command) {
    auto path = workspace.file_table.resolve(unit);
    return workspace.build.resolve(unit, command.config, command.source, path, path);
}

/// Whether a file of `family` can be part of a command's translation
/// unit. CUDA and Objective-C++ units are C++ with more, so a C++ header
/// fits them (a `.cuh` or `.mm` needs its own); a C++ source borrowing
/// such a command would compile as that language, so only headers get
/// that latitude.
bool compatible(Language file, Language command, bool header) {
    return file == Language::Any || file == command ||
           (header && file == Language::CXX &&
            (command == Language::CUDA || command == Language::ObjCXX));
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
    index.missing.clear();
    auto members = workspace.build.members();
    std::ranges::sort(members, {}, [&](Fid unit) { return workspace.file_table.resolve(unit); });
    for(auto member: members) {
        // A member a rule claims with a default command that is no compile
        // command has none; a listed unit deleted from disk lends nothing.
        if(!llvm::sys::fs::exists(workspace.file_table.resolve(member))) {
            index.missing.insert(member);
            continue;
        }
        for(auto& command: workspace.build.commands(member)) {
            auto ref = effective(workspace, member, command);
            auto position = static_cast<std::uint32_t>(index.commands.size());
            index.commands.push_back({
                .lender = {.unit = member, .config = command.config},
                .family = family_of_command(ref)
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
    auto family = family_of_suffix(path);
    bool header = header_suffix(path);
    auto dir = path::parent_path(path);
    auto stem = path::stem(path);
    auto& index = lender_index(workspace);
    auto fits = [&](const LenderIndex::Command& command) {
        return compatible(family, command.family, header);
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
        if(!commands.empty() &&
           (family == Language::Any ||
            compatible(family,
                       family_of_command(effective(workspace, candidate, commands.front())),
                       /*header=*/true))) {
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

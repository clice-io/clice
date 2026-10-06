#include "project/hosting.h"

#include <algorithm>
#include <tuple>
#include <utility>

#include "project/project.h"
#include "vfs/path.h"

#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/Path.h"
#include "clang/Driver/Types.h"

namespace clice {

namespace {

namespace types = clang::driver::types;

/// The language a command compiles its unit as — a `-x` in the entry or
/// a rule's append included, else the unit's suffix as its driver reads
/// it (`g++` takes a `.c` as C++).
types::ID language_of(const CommandRef& command) {
    return types::lookupTypeForTypeSpecifier(command.input.value);
}

CommandRef effective(Project& project, Fid unit, const Candidate& command) {
    auto path = project.file_table.resolve(unit);
    return project.build.resolve(unit, command.config, command.source, path, path);
}

/// Whether the file at `path` can be part of the translation unit `unit`
/// compiled as `language`. A source only in its own, or in the one the
/// unit's command gives the unit's own suffix (`g++` takes a `.c` as
/// C++): rendering the borrowed command for it would otherwise force
/// `-x`, and a `.cpp` compiled as CUDA or a `.m` as C is not the file. A
/// header has latitude: a `.h` fits any, a C++ header every language
/// built on C++ (Objective-C++, CUDA, HIP), a `.cuh` CUDA.
bool compatible(llvm::StringRef path, llvm::StringRef unit, types::ID language) {
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
    return file == language ||
           (file == suffix_type(unit) && language == types::lookupCXXTypeForCType(file));
}

/// How far `other` sits from `path` in the directory tree: how much of
/// `path` lies past the directories the two share, then how many
/// directories deeper `other` goes below them.
std::pair<std::size_t, std::size_t> tree_distance(llvm::StringRef path, llvm::StringRef other) {
    std::size_t shared = 0;
    auto n = std::min(path.size(), other.size());
    while(shared < n && path[shared] == other[shared]) {
        shared += 1;
    }
    auto separator = [](char c) {
        return path::is_separator(c);
    };
    while(shared > 0 && !separator(path[shared - 1])) {
        shared -= 1;
    }
    return {path.size() - shared,
            static_cast<std::size_t>(llvm::count_if(other.substr(shared), separator))};
}

/// enterings() by one include tree, rooted at `root`.
std::optional<llvm::SmallVector<Host>> tree_enterings(Project& project,
                                                      Fid host,
                                                      Fid header,
                                                      VersionID root,
                                                      llvm::ArrayRef<index::IncludeNode> nodes) {
    using Verdict = vfs::DiskState::Verdict;
    auto& files = project.file_table;
    if(files.check_version(root) != Verdict::Fresh) {
        return std::nullopt;
    }
    auto file_of = [&](std::uint32_t node) {
        return files.version(VersionID{nodes[node].file}).fid;
    };
    auto fresh = [&](std::uint32_t node) {
        return files.check_version(VersionID{nodes[node].file}) == Verdict::Fresh;
    };

    llvm::SmallVector<Host> found;
    bool forced = false;
    for(std::uint32_t i = 0; i < nodes.size(); i += 1) {
        if(nodes[i].skipped || file_of(i) != header) {
            continue;
        }
        llvm::SmallVector<std::uint32_t> path;
        for(auto node = i; node != index::no_node; node = nodes[node].parent) {
            path.push_back(node);
        }
        // A file the command forces in hangs off the unit like its own
        // directives, at a line of the command-line buffer.
        if(llvm::is_contained(project.dep_graph.get_forcing_units(file_of(path.back())), host)) {
            forced = true;
            continue;
        }
        Host entering{.file = host, .chain = {host}};
        for(auto node: llvm::reverse(path)) {
            if(node != i && !fresh(node)) {
                return std::nullopt;
            }
            entering.chain.push_back(file_of(node));
            entering.lines.push_back(nodes[node].line);
        }
        found.push_back(std::move(entering));
    }
    if(forced && found.empty()) {
        return std::nullopt;
    }
    if(!found.empty()) {
        // Nodes are numbered by includer, not by when the compile entered
        // them; within one file the directives run top to bottom, so the
        // first line where two chains part orders them.
        llvm::sort(found, [](const Host& a, const Host& b) {
            return std::ranges::lexicographical_compare(a.lines, b.lines);
        });
        return found;
    }

    auto chain = project.dep_graph.find_include_chain(host, header);
    for(auto file: llvm::ArrayRef(chain).drop_back(chain.empty() ? 0 : 1)) {
        for(std::uint32_t i = 0; i < nodes.size(); i += 1) {
            if(file_of(i) == file && !fresh(i)) {
                return std::nullopt;
            }
        }
    }
    return found;
}

const LenderIndex& lender_index(Project& project) {
    auto& index = project.lenders;
    if(index.epoch == project.commands_epoch) {
        return index;
    }
    index.commands.clear();
    index.search_dirs.clear();
    auto members = project.build.members();
    std::ranges::sort(members, {}, [&](Fid unit) { return project.file_table.resolve(unit); });
    llvm::StringMap<CanonicalPath> identities;
    for(auto member: members) {
        // A member a rule claims with a default command that is no compile
        // command has none.
        for(auto& command: project.build.commands(member)) {
            auto ref = effective(project, member, command);
            auto position = static_cast<std::uint32_t>(index.commands.size());
            index.commands.push_back({
                .lender = {.unit = member, .config = command.config},
                .language = language_of(ref),
            });
            for(auto& search_dir: project.cdb.search_config(ref).dirs) {
                auto [it, inserted] = identities.try_emplace(search_dir.path);
                if(inserted) {
                    it->second = CanonicalPath(Spelling::absolute(search_dir.path));
                }
                index.search_dirs[it->second].push_back(position);
            }
        }
    }
    index.epoch = project.commands_epoch;
    return index;
}

}  // namespace

std::optional<Lender> command_lender(Project& project, Fid file) {
    auto& files = project.file_table;
    auto path = files.resolve(file);
    bool header = is_header_path(path);
    auto dir = path::parent_path(path);
    auto stem = path::stem(path);
    auto& index = lender_index(project);
    auto fits = [&](const LenderIndex::Command& command) {
        return compatible(path, files.resolve(command.lender.unit), command.language);
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

    // The closest unit in the directory tree, then by name.
    return *std::ranges::min_element(units, {}, [&](const Lender& lender) {
        return std::tuple(tree_distance(path, unit_path(lender)), unit_path(lender));
    });
}

std::optional<llvm::SmallVector<Host>> enterings(Project& project, Fid host, Fid header) {
    if(auto it = project.project_index.manifests.find(host);
       it != project.project_index.manifests.end()) {
        if(auto found = tree_enterings(project, host, header, it->second.tu_fv, it->second.nodes)) {
            return found;
        }
    }
    if(auto it = project.include_trees.find(host);
       it != project.include_trees.end() && it->second.root.valid() &&
       it->second.commands_epoch == project.commands_epoch) {
        return tree_enterings(project, host, header, it->second.root, it->second.nodes);
    }
    return std::nullopt;
}

std::uint32_t count_occurrences(Project& project, Fid host, Fid header) {
    if(auto found = enterings(project, host, header)) {
        return static_cast<std::uint32_t>(found->size());
    }
    auto chain = project.dep_graph.find_include_chain(host, header);
    if(chain.size() < 2) {
        return 0;
    }
    return project.dep_graph.count_includes(chain[chain.size() - 2], header);
}

llvm::SmallVector<Candidate, 2> host_commands(Project& project, Fid header, Fid host) {
    auto header_path = project.file_table.resolve(header);
    auto host_path = project.file_table.resolve(host);
    llvm::SmallVector<Candidate, 2> fitting;
    for(auto& command: project.build.commands(host)) {
        if(compatible(header_path, host_path, language_of(effective(project, host, command)))) {
            fitting.push_back(command);
        }
    }
    return fitting;
}

llvm::SmallVector<Fid> ranked_hosts(Project& project, Fid header) {
    auto& files = project.file_table;
    auto header_path = files.resolve(header);
    auto header_stem = llvm::sys::path::stem(header_path);
    auto header_dir = llvm::sys::path::parent_path(header_path);
    auto sources = project.build.source_order(header_path);

    // The lexical scan follows each header's includes under the first
    // command that reached it; a unit whose compile resolved them
    // otherwise names the header in its index rows.
    llvm::SmallVector<Fid> hosts;
    llvm::DenseSet<Fid> seen;
    auto add = [&](Fid candidate) {
        if(seen.insert(candidate).second && !host_commands(project, header, candidate).empty()) {
            hosts.push_back(candidate);
        }
    };
    for(auto candidate: project.dep_graph.find_host_sources(header)) {
        add(candidate);
    }
    project.project_index.each_contributor(header, add);

    auto score =
        [&](Fid host) -> std::tuple<std::size_t, int, int, std::pair<std::size_t, std::size_t>> {
        auto host_path = files.resolve(host);
        // A host compiled from a database the header's rules name comes
        // first; one living on a default command comes after every
        // database.
        std::size_t source_rank = sources.size();
        if(auto entries = project.build.entries(host); !entries.empty()) {
            source_rank = llvm::find(sources, entries.front().source) - sources.begin();
        }
        int stem_match = llvm::sys::path::stem(host_path) == header_stem ? 0 : 1;
        int same_dir = llvm::sys::path::parent_path(host_path) == header_dir ? 0 : 1;
        return {source_rank, stem_match, same_dir, tree_distance(header_path, host_path)};
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

std::optional<Host> default_host(Project& project, Fid header) {
    for(auto host: ranked_hosts(project, header)) {
        if(auto found = enterings(project, host, header)) {
            if(!found->empty()) {
                return std::move(found->front());
            }
            continue;
        }
        auto chain = project.dep_graph.find_include_chain(host, header);
        if(!chain.empty()) {
            return Host{.file = host, .chain = std::move(chain)};
        }
    }
    return std::nullopt;
}

}  // namespace clice

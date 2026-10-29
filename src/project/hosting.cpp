module clice;

import :command.file_kind;
import :command.search_config;
import :compile.diagnostic;
import :index.include_tree;
import :index.project_index;
import :index.tu_manifest;
import :project.command_resolver;
import :project.hosting;
import :project.project;
import :support.logging;
import :syntax.include_resolver;
import :vfs.file_system;
import :vfs.path;

namespace clice {

namespace {

namespace types = clang::driver::types;

/// The language a command compiles its unit as — a `-x` in the entry or
/// a rule's append included, else the unit's suffix as its driver reads
/// it (`g++` takes a `.c` as C++).
types::ID language_of(const CommandRef& command) {
    return types::lookupTypeForTypeSpecifier(command.input.value);
}

/// Whether `type` is C++ or a C++ module unit: the two share flags, so
/// each lends to the other.
bool cxx_or_module(types::ID type) {
    return type == types::TY_CXX || type == types::TY_CXXModule;
}

/// Whether the file at `path` can be part of the translation unit `unit`
/// compiled as `language`. A source only in its own, in the one the
/// unit's command gives the unit's own suffix (`g++` takes a `.c` as
/// C++), or across C++ and its modules: a `.cpp` compiled as CUDA or a
/// `.m` as C is not the file. A header has latitude: a `.h` fits any, a
/// C++ header every language built on C++ (Objective-C++, CUDA, HIP), a
/// `.cuh` CUDA.
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
    return file == language || (cxx_or_module(file) && cxx_or_module(language)) ||
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
            static_cast<std::size_t>(ranges::count_if(other.substr(shared), separator))};
}

/// include_chains() by one include tree, rooted at `root`.
std::optional<llvm::SmallVector<Host>> tree_chains(Project& project,
                                                   Fid host,
                                                   Fid header,
                                                   VersionID root,
                                                   llvm::ArrayRef<index::IncludeNode> nodes,
                                                   llvm::ArrayRef<VersionID> absent) {
    using CheckResult = vfs::DiskState::CheckResult;
    auto& files = project.file_table;
    if(files.check_version(root) != CheckResult::Fresh ||
       ranges::any_of(absent, [&](VersionID place) {
           return files.disk.present(files.version(place).fid);
       })) {
        return std::nullopt;
    }
    auto file_of = [&](std::uint32_t node) {
        return files.version(VersionID{nodes[node].file}).fid;
    };
    auto fresh = [&](std::uint32_t node) {
        return files.check_version(VersionID{nodes[node].file}) == CheckResult::Fresh;
    };

    // The header's own guard decides which of its includes after the first
    // enter it.
    // FIXME: another file defining the same guard skips the first include too:
    // a header included once is not checked, so fixing its guard keeps the
    // tree's answer until the host's tree is taken again.
    auto repeated = ranges::count_if(nodes, [&](const index::IncludeNode& node) {
                        return files.version(VersionID{node.file}).fid == header;
                    }) > 1;

    struct Chain {
        /// The compile enters what its command forces in ahead of the
        /// unit's text, in the command's order: a forced chain sorts by
        /// its line in the command-line buffer, before every other.
        llvm::SmallVector<std::uint32_t> order;

        Host host;
    };

    llvm::SmallVector<Chain> found;
    llvm::DenseSet<Fid> named;
    for(std::uint32_t i = 0; i < nodes.size(); i += 1) {
        named.insert(file_of(i));
        if(nodes[i].skipped || file_of(i) != header) {
            continue;
        }
        llvm::SmallVector<std::uint32_t> path;
        for(auto node = i; node != index::no_node; node = nodes[node].parent) {
            path.push_back(node);
        }
        // A file the command forces in hangs off the unit like its own
        // directives, at a line of the command-line buffer: the chain
        // starts at it.
        auto root = path.back();
        // FIXME: by file, not by node: a file without a guard that the unit both
        // forces in and includes at its top level is taken for the forced one at
        // either root.
        bool forced = ranges::contains(project.dep_graph.get_forcing_units(file_of(root)), host);
        Chain chain{.host = {.file = host}};
        chain.order = forced ? llvm::SmallVector<std::uint32_t>{0, nodes[root].line}
                             : llvm::SmallVector<std::uint32_t>{1};
        if(!forced) {
            chain.host.chain.push_back(host);
        }
        for(auto node: views::reverse(path)) {
            if((node != i || repeated) && !fresh(node)) {
                return std::nullopt;
            }
            chain.host.chain.push_back(file_of(node));
            if(!forced || node != root) {
                chain.host.lines.push_back(nodes[node].line);
            }
        }
        chain.order.append(chain.host.lines.begin(), chain.host.lines.end());
        found.push_back(std::move(chain));
    }
    // Nodes are numbered by includer, not by when the compile entered
    // them; within one file the directives run top to bottom, so the
    // first line where two chains part orders them.
    ranges::sort(found, [](const Chain& a, const Chain& b) {
        return ranges::lexicographical_compare(a.order, b.order);
    });
    llvm::SmallVector<Host> hosts;
    for(auto& chain: found) {
        hosts.push_back(std::move(chain.host));
    }
    if(!hosts.empty()) {
        return hosts;
    }

    // A forced file a precompiled header stands for, and everything it
    // includes, was entered before the tree began: the tree does not name
    // it, nor tell whether the compile enters the header through it.
    auto& graph = project.dep_graph;
    for(auto file: graph.get_forced_includes(host)) {
        if(!named.contains(file) && !graph.find_include_chain(file, header).empty()) {
            return std::nullopt;
        }
    }

    // The files of the scan's chains to the header, from the unit and from
    // what it forces in, which may have gained the include since.
    llvm::DenseSet<Fid> on_chain;
    for(auto from: llvm::concat<const Fid>(llvm::ArrayRef(host), graph.get_forced_includes(host))) {
        auto chain = graph.find_include_chain(from, header);
        on_chain.insert(chain.begin(), chain.end());
    }
    on_chain.erase(header);
    for(std::uint32_t i = 0; i < nodes.size(); i += 1) {
        if(on_chain.contains(file_of(i)) && !fresh(i)) {
            return std::nullopt;
        }
    }
    return hosts;
}

const LenderIndex& lender_index(Project& project) {
    auto& index = project.hosting.lenders;
    if(index.epoch == project.commands_epoch) {
        return index;
    }
    index.commands.clear();
    index.search_dirs.clear();
    auto members = project.build.members();
    ranges::sort(members, {}, [&](Fid unit) { return project.file_table.resolve(unit); });
    llvm::StringMap<CanonicalPath> identities;
    for(auto member: members) {
        // A member a rule covers with a default command that is no compile
        // command has none; a provisional member's is borrowed, and never
        // lent on.
        for(auto& candidate: project.build.commands(member)) {
            if(candidate.source == CommandSource::Inferred) {
                continue;
            }
            auto ref = tree_request(project, member, candidate).command;
            auto position = static_cast<std::uint32_t>(index.commands.size());
            index.commands.push_back({
                .lender = {.unit = member, .command = candidate.command},
                .language = language_of(ref),
            });
            for(auto& search_dir: project.toolchain.search_config(ref).dirs) {
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

TreeRequest tree_request(Project& project, Fid unit, const Candidate& candidate) {
    auto path = project.file_table.resolve(unit);
    auto command = project.build.resolve(unit, candidate.command, candidate.source, path, path);
    return {
        .key = {unit, project.commands.command_hash(command.command)},
        .command = command
    };
}

std::optional<llvm::SmallVector<Host>> include_chains(Project& project,
                                                      const UnitCommand& unit,
                                                      Fid header) {
    auto [host, command] = unit;
    // The index runs a unit under its first command.
    if(auto commands = project.build.commands(host);
       !commands.empty() && tree_request(project, host, commands.front()).key.second == command) {
        if(auto manifest = project.index.manifest(host)) {
            if(auto found = tree_chains(project,
                                        host,
                                        header,
                                        manifest->tu_version,
                                        manifest->nodes,
                                        manifest->absent)) {
                return found;
            }
        }
    }
    auto* entry = project.hosting.trees.lookup(project.file_table, unit);
    if(!entry) {
        return std::nullopt;
    }
    if(!entry->result.root.valid()) {
        return llvm::SmallVector<Host>();
    }
    return tree_chains(project, host, header, entry->result.root, entry->result.nodes, {});
}

llvm::SmallVector<Candidate, 2> host_commands(Project& project, Fid header, Fid host) {
    auto header_path = project.file_table.resolve(header);
    auto host_path = project.file_table.resolve(host);
    llvm::SmallVector<Candidate, 2> fitting;
    for(auto& candidate: project.build.commands(host)) {
        auto command = tree_request(project, host, candidate).command;
        if(!project.commands.pch_output(command.command) &&
           compatible(header_path, host_path, language_of(command))) {
            fitting.push_back(candidate);
        }
    }
    return fitting;
}

llvm::SmallVector<Fid> ranked_hosts(Project& project, Fid header) {
    std::array generations = {project.dep_graph.generation(),
                              project.commands_epoch,
                              project.index.manifest_generation()};
    auto [it, inserted] = project.hosting.rankings.try_emplace(header);
    auto& ranking = it->second;
    if(!inserted && ranking.generations == generations) {
        return ranking.hosts;
    }
    auto& files = project.file_table;
    auto header_path = files.resolve(header);
    auto header_stem = path::stem(header_path);
    auto header_dir = path::parent(header_path);
    auto sources = project.build.source_order(header_path);

    // The lexical scan follows each header's includes under the first
    // command that reached it; a unit whose compile resolved them
    // otherwise names the header in its index entries.
    using Score =
        std::tuple<std::size_t, int, int, std::pair<std::size_t, std::size_t>, std::string>;
    llvm::SmallVector<std::pair<Score, Fid>> scored;
    llvm::DenseSet<Fid> seen;
    auto add = [&](Fid candidate) {
        // A header with an entry of its own contributes to itself.
        if(candidate == header || !seen.insert(candidate).second ||
           host_commands(project, header, candidate).empty()) {
            return;
        }
        auto host_path = files.resolve(candidate);
        // A host compiled from a database the header's rules name comes
        // first; one living on a default command comes after every
        // database.
        std::size_t source_rank = sources.size();
        if(auto entries = project.build.entries(candidate); !entries.empty()) {
            source_rank = ranges::find(sources, entries.front().source) - sources.begin();
        }
        int stem_match = path::stem(host_path) == header_stem ? 0 : 1;
        int same_dir = path::parent(host_path) == header_dir ? 0 : 1;
        scored.push_back({
            {source_rank,
             stem_match, same_dir,
             tree_distance(header_path, host_path),
             host_path.str()},
            candidate
        });
    };
    // A source another one includes still compiles on its own.
    auto unit = [&](Fid file) {
        return project.build.unit(file);
    };
    for(auto candidate: project.dep_graph.find_readers(header, unit)) {
        add(candidate);
    }
    project.index.for_each_contributor(header, add);
    ranges::sort(scored);

    ranking = {
        .hosts = ranges::to<llvm::SmallVector<Fid>>(views::values(scored)),
        .generations = generations,
    };
    return ranking.hosts;
}

HostSearch find_host(Project& project, Fid header, const Selection* pin) {
    auto search = [&](Fid host,
                      const Candidate& candidate,
                      std::uint32_t occurrence) -> std::optional<HostSearch> {
        auto request = tree_request(project, host, candidate);
        auto chains = include_chains(project, request.key, header);
        if(!chains) {
            return HostSearch{.missing = std::move(request)};
        }
        if(occurrence < chains->size()) {
            return HostSearch{.host = std::move((*chains)[occurrence]), .occurrence = occurrence};
        }
        return std::nullopt;
    };

    // The pin holds while its host's compile enters the header where it
    // says; otherwise the header compiles as if nothing were pinned.
    if(pin && pin->host_path_id.valid()) {
        auto host = pin->host_path_id;
        if(auto picked =
               pinned_host_command(project, header, host, pin->command_hash, pin->base_hash)) {
            if(auto found = search(host, *picked, pin->occurrence.value_or(0))) {
                return std::move(*found);
            }
        }
    }
    for(auto host: ranked_hosts(project, header)) {
        if(auto found = search(host, host_commands(project, header, host).front(), 0)) {
            return std::move(*found);
        }
    }
    return {};
}

Fid host_of(Project& project, Fid header) {
    auto search = find_host(project, header, nullptr);
    if(search.missing) {
        return search.missing->key.first;
    }
    return search.host ? search.host->file : Fid{};
}

std::optional<std::uint32_t> count_occurrences(Project& project, Fid host, Fid header) {
    auto commands = host_commands(project, header, host);
    if(commands.empty()) {
        return 0;
    }
    auto chains =
        include_chains(project, tree_request(project, host, commands.front()).key, header);
    if(!chains) {
        return std::nullopt;
    }
    return static_cast<std::uint32_t>(chains->size());
}

std::optional<bool> self_contained(Project& project, const TrialKey& key, bool open) {
    auto& files = project.file_table;
    // An X-macro list and the like compile only inside their includer.
    if(is_context_header_path(files.resolve(key.first))) {
        return false;
    }
    auto* entry = project.hosting.trials.lookup(files, key);
    if(!entry) {
        return std::nullopt;
    }
    // A result the other side does not take is kept for it. The editor's
    // resolutions look at the disk through what the tracker saw last, as
    // they read the buffer, not the file.
    if(open) {
        auto seen = files.disk.seen_hash(key.first);
        if(!seen || *seen != files.version(entry->result.disk).content_hash) {
            return std::nullopt;
        }
    } else if(files.check_version(entry->result.text) != vfs::DiskState::CheckResult::Fresh) {
        return std::nullopt;
    }
    return entry->result.self_contained;
}

std::size_t missing_context_errors(llvm::ArrayRef<feature::FileDiagnostic> diagnostics) {
    namespace diag = clang::diag;
    constexpr static unsigned ids[] = {
        diag::err_unknown_typename,
        diag::err_unknown_typename_suggest,
        diag::err_undeclared_var_use,
        diag::err_undeclared_var_use_suggest,
        diag::err_undeclared_use,
        diag::err_undeclared_use_suggest,
        diag::err_no_member,
        diag::err_no_member_suggest,
        diag::err_no_member_template,
        diag::err_no_member_template_suggest,
        diag::err_no_template,
        diag::err_no_template_suggest,
        diag::err_typename_nested_not_found,
        diag::err_unknown_nested_typename_suggest,
        diag::ext_implicit_function_decl_c99,
        diag::err_pp_hash_error,
        diag::err_pp_unterminated_conditional,
    };
    return ranges::count_if(diagnostics, [&](const feature::FileDiagnostic& diagnostic) {
        return diagnostic.level >= DiagnosticLevel::Error && ranges::contains(ids, diagnostic.id);
    });
}

void commit_trial(Project& project,
                  const TrialKey& key,
                  llvm::ArrayRef<feature::FileDiagnostic> diagnostics,
                  VersionID text,
                  DepsSnapshot trace) {
    auto disk = current_dep(project.file_table, key.first);
    project.hosting.trials.commit(key,
                                  {
                                      .self_contained = missing_context_errors(diagnostics) == 0,
                                      .text = text,
                                      // A header gone from disk holds no result.
                                      .disk = disk ? disk->version : text,
                                  },
                                  std::move(trace));
    // A header found self-contained lists hosts lending the same command
    // once (ContextService::contexts).
    project.context_epoch += 1;
    project.mark_artifacts_dirty();
}

bool saved_trial(Project& project, const TrialKey& key, VersionID text, bool leans) {
    auto* entry = project.hosting.trials.find(key);
    if(!entry) {
        return false;
    }
    project.mark_artifacts_dirty();
    if(entry->result.self_contained && leans) {
        project.hosting.trials.forget(key);
        project.context_epoch += 1;
        return true;
    }
    entry->result.text = text;
    entry->result.disk = text;
    return false;
}

void commit_tree(Project& project,
                 const TreeRequest& request,
                 llvm::ArrayRef<std::string> paths,
                 llvm::ArrayRef<std::uint64_t> hashes,
                 std::vector<index::IncludeNode> nodes,
                 llvm::ArrayRef<DepFile> deps,
                 std::int64_t build_at) {
    auto& files = project.file_table;
    llvm::SmallVector<VersionID> versions;
    for(std::size_t i = 0; i < paths.size(); i += 1) {
        auto fid = files.intern(Spelling::absolute(paths[i]));
        if(hashes[i] != 0) {
            files.disk.consumed(fid, hashes[i]);
        }
        versions.push_back(files.intern_version(fid, hashes[i]));
    }
    for(auto& node: nodes) {
        node.file = versions[node.file].raw;
    }
    // The unit's own text is no dependency of its compile, but the tree's
    // root.
    auto trace = capture_deps_snapshot(files, deps, build_at);
    trace.push_back({.path_id = request.key.first, .version = versions.back()});
    project.hosting.trees.commit(request.key,
                                 {.root = versions.back(), .nodes = std::move(nodes)},
                                 std::move(trace));
    // The tree can move the host's contexts: the listings it changes are
    // stale.
    project.context_epoch += 1;
}

void commit_failed_tree(Project& project, const TreeRequest& request) {
    DepsSnapshot trace;
    if(auto unit = current_dep(project.file_table, request.key.first)) {
        trace.push_back(*unit);
    }
    project.hosting.trees.commit(request.key, {}, std::move(trace));
    project.context_epoch += 1;
}

void take_tree(Project& project, const TreeRequest& request) {
    auto& command = request.command;
    auto rendered = project.toolchain.render(command, project.cdb.spelling(command.file));
    auto unit = clice::compile({
        .kind = CompilationKind::Preprocess,
        .input = {.arguments = {rendered.argv.begin(), rendered.argv.end()},
                  .directory = project.commands.get(command.command).directory,
                  .workspace = project.config.workspace_root.str(),
                  .vfs = llvm::makeIntrusiveRefCnt<vfs::View>(std::move(rendered.hidden))},
    });
    if(!unit->completed()) {
        commit_failed_tree(project, request);
        return;
    }
    auto tree = index::IncludeTree::from(*unit);
    commit_tree(project,
                request,
                tree.paths,
                tree.content_hashes,
                std::move(tree.nodes),
                unit->deps(),
                unit->build_at().count());
}

Memo<Fid, HeaderContext>::Entry
    synthesize(Project& project, Fid header, const Host& host, const CommandRef& command) {
    auto& files = project.file_table;
    auto& contexts = project.hosting.contexts;
    auto command_hash = project.commands.command_hash(command.command);
    if(auto* entry = contexts.lookup(files, header)) {
        auto& known = entry->result;
        if(known.host == host.file && known.command == command_hash && known.chain == host.chain &&
           known.lines == host.lines) {
            return *entry;
        }
    }
    HeaderContext context{
        .host = host.file,
        .command = command_hash,
        .chain = host.chain,
        .lines = host.lines,
    };
    auto& chain = host.chain;

    // Include directives along the chain are resolved with the host's real
    // search configuration, so same-named headers in different directories
    // cannot be confused.
    auto search_config = project.toolchain.search_config(command);
    vfs::Scope scope(files.dirs);
    auto resolved_config = resolve_search_config(search_config, scope);
    auto resolver =
        [&](const ScanResult::IncludeInfo& include,
            llvm::StringRef includer_dir,
            std::optional<unsigned> includer_found_dir) -> std::optional<ResolveResult> {
        auto result = resolve_include(include.path,
                                      include.is_angled,
                                      &scope.list(includer_dir),
                                      includer_dir,
                                      include.is_include_next,
                                      includer_found_dir,
                                      resolved_config,
                                      scope);
        if(!result) {
            return std::nullopt;
        }
        // Chain files are named by the build's spelling of each, so a
        // resolution matches the next one exactly when it is that file.
        auto found = files.intern(Spelling::absolute(result->path));
        result->path = files.spelling(found).str();
        return result;
    };

    // The versions name the bytes just read — the bytes the synthesized
    // context embeds.
    std::vector<std::string> contents;
    std::vector<Spelling> spellings;
    llvm::SmallVector<ChainEntry> entries;
    DepsSnapshot trace;
    contents.reserve(chain.size() - 1);
    spellings.reserve(chain.size() - 1);
    auto commit = [&] {
        return contexts.commit(header, std::move(context), std::move(trace));
    };
    for(std::size_t i = 0; i + 1 < chain.size(); i += 1) {
        auto path = files.resolve(chain[i]);
        auto observed = vfs::read_observed(path);
        if(!observed) {
            LOG_WARN("Cannot read {} for the context of {}", path, files.resolve(header));
            if(auto missing = current_dep(files, chain[i])) {
                trace.push_back(*missing);
            }
            return commit();
        }
        contents.push_back(std::move(observed->text));
        spellings.push_back(files.spelling(chain[i]));
        entries.push_back(
            {.path = spellings.back(), .content = contents.back(), .line = host.lines[i]});
        files.disk.observe(chain[i], observed->obs);
        trace.push_back(
            {.path_id = chain[i], .version = files.intern_version(chain[i], observed->obs.hash)});
    }

    // Snapshot the header itself for other occurrences along the chain:
    // its real path is remapped to the open buffer at compile time, so
    // includes of it inside the context must point at a copy. The snapshot
    // mirrors the header's disk state; a context embedding it is
    // synthesized again when it changes so other-occurrence expansions
    // stay current.
    std::optional<llvm::StringRef> target_content;
    auto target_observed = vfs::read_observed(files.resolve(header));
    if(target_observed) {
        target_content = target_observed->text;
        files.disk.observe(header, target_observed->obs);
    }
    auto synthesized =
        synthesize_context(entries, files.spelling(header), resolver, target_content);
    if(!synthesized) {
        LOG_WARN("Cannot match the include chain of {} under {}",
                 files.resolve(header),
                 files.resolve(host.file));
        return commit();
    }
    if(synthesized->snapshot) {
        trace.push_back({.path_id = header,
                         .version = files.intern_version(header, target_observed->obs.hash)});
    }
    context.synthesized = std::make_shared<const SynthesizedContext>(std::move(*synthesized));
    return commit();
}

std::optional<Lender> command_lender(Project& project, Fid file) {
    auto& files = project.file_table;
    auto path = files.resolve(file);
    bool header = is_header_path(path);
    auto dir = path::parent(path);
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

    auto siblings =
        ranges::to<llvm::SmallVector<Lender>>(views::filter(units, [&](const Lender& lender) {
            return path::parent(unit_path(lender)) == dir;
        }));
    if(!siblings.empty()) {
        return *ranges::min_element(siblings, {}, [&](const Lender& lender) {
            return std::tuple(path::stem(unit_path(lender)) != stem, unit_path(lender));
        });
    }

    // A header some command's header search reaches: that unit's code
    // finds it by that path, so the command is the one the header is
    // written for. Nearest directory first, by path and command within.
    if(header) {
        for(auto ancestor: path::ancestors(dir)) {
            auto it = index.search_dirs.find(ancestor);
            if(it == index.search_dirs.end()) {
                continue;
            }
            for(auto position: it->second) {
                if(fits(index.commands[position])) {
                    return index.commands[position].lender;
                }
            }
        }
    }

    // The closest unit in the directory tree, then by name.
    return *ranges::min_element(units, {}, [&](const Lender& lender) {
        return std::tuple(tree_distance(path, unit_path(lender)), unit_path(lender));
    });
}

}  // namespace clice

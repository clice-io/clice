module clice;

import :command.argument;
import :command.file_kind;
import :command.search_config;
import :index.project_index;
import :project.command_resolver;
import :project.hosting;
import :support.logging;
import :syntax.header_context;
import :syntax.include_resolver;
import :syntax.lexer;
import :vfs.file_system;
import :vfs.file_table;
import :vfs.path;

namespace clice {

namespace {

/// Per-file command selection decision log: which tiers were tried, which one
/// was hit, and a hash of the final command for correlating later failures.
void log_command_decision(llvm::StringRef path,
                          llvm::ArrayRef<llvm::StringRef> tried,
                          CommandSource source,
                          llvm::ArrayRef<std::string> arguments) {
    if(logging::options.level > logging::Level::info)
        return;
    std::string joined;
    for(auto& arg: arguments) {
        joined += arg;
        joined += '\0';
    }
    LOG_INFO("compile_args: file={} tried=[{}] source={} args_hash={:016x}",
             path,
             llvm::join(tried, ","),
             source,
             llvm::xxh3_64bits(llvm::StringRef(joined)));
}

/// How many of `ref`'s `-include`s come before the one naming `forced`:
/// what a header context entered through that forced file keeps
/// (Resolution::forced). All of them for none.
std::size_t includes_before(Project& project, const CommandRef& ref, Fid forced) {
    if(!forced.valid()) {
        return std::numeric_limits<std::size_t>::max();
    }
    auto search = project.toolchain.search_config(ref);
    vfs::Scope scope(project.file_table.dirs);
    auto resolved = resolve_search_config(search, scope);
    llvm::StringRef directory = project.commands.get(ref.command).directory;
    for(std::size_t i = 0; i < search.forced_includes.size(); i += 1) {
        auto header = resolve_forced_include(search.forced_includes[i], directory, resolved, scope);
        if(header && project.file_table.find(Spelling::absolute(header->path)) == forced) {
            return i;
        }
    }
    return std::numeric_limits<std::size_t>::max();
}

/// The language of the command a file compiles with — among a
/// multi-configuration file's or host's entries the pinned one, else the
/// first, or the default command covering it — rules applied, like the
/// resolve path's effective command: whether it compiles the file as C,
/// whether an -x forces that (the driver override beats every suffix
/// heuristic), and the last -std value.
struct Language {
    CommandSource source;
    bool is_c = false;
    bool language_forced = false;
    std::string standard;
};

std::optional<Language> language_of(Project& project,
                                    Fid file,
                                    llvm::ArrayRef<CanonicalRef> paths,
                                    const Selection* pin) {
    auto path = project.file_table.resolve(file);
    auto commands = project.build.commands(file);
    if(commands.empty()) {
        return std::nullopt;
    }
    auto candidate = pick_pinned_command(project,
                                         file,
                                         commands,
                                         paths,
                                         path,
                                         pin ? llvm::StringRef(pin->command_hash) : "",
                                         pin ? llvm::StringRef(pin->base_hash) : "");
    auto ref = project.build.resolve(file, candidate.command, candidate.source, paths, path);

    llvm::StringRef language = ref.input.value;
    Language result{
        .source = candidate.source,
        .is_c = language == "c" || language == "c-header",
        .language_forced = !project.commands.forced_language(ref.command).empty(),
    };
    for(auto& arg: project.commands.get(ref.command).args) {
        if(arg.opt_id == option::OPT_std_EQ && arg.values.size() == 1) {
            result.standard = arg.values[0];
        }
    }
    return result;
}

}  // namespace

Candidate pick_pinned_command(Project& project,
                              Fid file,
                              llvm::ArrayRef<Candidate> candidates,
                              llvm::ArrayRef<CanonicalRef> paths,
                              llvm::StringRef language_path,
                              llvm::StringRef pinned_hash,
                              llvm::StringRef pinned_base) {
    // The base identity resolved at pin time is exact; the applied hash
    // remains as the fallback for pins saved before the base was recorded
    // (and cannot distinguish candidates the rules collapse together).
    if(!pinned_base.empty()) {
        for(auto& entry: candidates) {
            if(project.commands.command_hash_hex(entry.command) == pinned_base) {
                return entry;
            }
        }
    }
    if(!pinned_hash.empty()) {
        for(auto& entry: candidates) {
            auto ref =
                project.build.resolve(file, entry.command, entry.source, paths, language_path);
            if(project.commands.command_hash_hex(ref.command) == pinned_hash) {
                return entry;
            }
        }
    }
    return candidates.front();
}

CommandRef lent_command(Project& project,
                        Fid header,
                        Fid host,
                        CommandID command,
                        llvm::ArrayRef<std::string> extra_prepend,
                        llvm::ArrayRef<std::string> extra_append) {
    // The header inherits the host's world: the rules matching the host
    // and the rules matching the header both edit the borrowed command,
    // each once, in declaration order.
    auto host_path = project.file_table.resolve(host);
    CanonicalRef edit_paths[] = {host_path, project.file_table.resolve(header)};
    return project.build.resolve(header,
                                 command,
                                 CommandSource::IncludeGraph,
                                 edit_paths,
                                 host_path,
                                 extra_prepend,
                                 extra_append);
}

std::optional<Candidate> pinned_host_command(Project& project,
                                             Fid header,
                                             Fid host,
                                             llvm::StringRef pinned_hash,
                                             llvm::StringRef pinned_base) {
    auto commands = host_commands(project, header, host);
    if(commands.empty()) {
        return std::nullopt;
    }
    auto host_path = project.file_table.resolve(host);
    CanonicalRef edit_paths[] = {host_path, project.file_table.resolve(header)};
    return pick_pinned_command(project,
                               header,
                               commands,
                               edit_paths,
                               host_path,
                               pinned_hash,
                               pinned_base);
}

std::optional<CommandRef> host_command(Project& project,
                                       Fid header,
                                       Fid host,
                                       llvm::StringRef pinned_hash,
                                       llvm::StringRef pinned_base,
                                       llvm::ArrayRef<std::string> extra_prepend,
                                       llvm::ArrayRef<std::string> extra_append) {
    auto picked = pinned_host_command(project, header, host, pinned_hash, pinned_base);
    if(!picked) {
        return std::nullopt;
    }
    return lent_command(project, header, host, picked->command, extra_prepend, extra_append);
}

const clang::LangOptions& dialect(Project& project, Fid file, const Selection* pin, Fid host) {
    auto path = project.file_table.resolve(file);
    bool host_pin = pin && pin->host_path_id.valid();
    auto own = language_of(project, file, path, host_pin ? nullptr : pin);
    // A file's entry is its command; a default command yields to the host
    // a header borrows from, in resolve_command's order.
    if(own && own->language_forced && own->source == CommandSource::CDBExact) {
        return dialect_of("", own->is_c, own->standard);
    }

    // A header's active context (the user's persisted choice, else the
    // resolved host) names the view being read; its command beats the
    // contributor union the way it does for the AST after an escalation.
    if(host.valid()) {
        CanonicalRef edit_paths[] = {project.file_table.resolve(host), path};
        auto host_lang = language_of(project, host, edit_paths, host_pin ? pin : nullptr);
        if(host_lang) {
            return dialect_of("", host_lang->is_c, host_lang->standard);
        }
    }

    if(own && own->language_forced) {
        return dialect_of("", own->is_c, own->standard);
    }

    bool contributed = false;
    bool c_only = true;
    project.index.for_each_contribution(file, [&](Fid tu, std::uint64_t) {
        contributed = true;
        c_only = c_only && llvm::StringRef(project.file_table.resolve(tu)).ends_with(".c");
    });
    c_only = contributed && c_only;
    return dialect_of(path, c_only, own ? llvm::StringRef(own->standard) : llvm::StringRef());
}

std::optional<CommandRef> CommandResolver::resolve_through_host(Fid path_id,
                                                                const CommandRequest& request,
                                                                Resolution& resolution) {
    auto search = find_host(project, path_id, request.selection);
    if(search.missing) {
        resolution.missing_tree = std::move(search.missing);
        return std::nullopt;
    }
    if(!search.host) {
        return std::nullopt;
    }
    auto& host = *search.host;
    auto* pin = request.selection;
    bool pinned = pin && pin->host_path_id == host.file;
    llvm::StringRef pinned_hash = pinned ? llvm::StringRef(pin->command_hash) : "";
    llvm::StringRef pinned_base = pinned ? llvm::StringRef(pin->base_hash) : "";
    // The host search ranks only hosts with a command fitting the header.
    auto ref = *host_command(project,
                             path_id,
                             host.file,
                             pinned_hash,
                             pinned_base,
                             request.extra_prepend,
                             request.extra_append);
    auto path = project.file_table.resolve(path_id);
    auto host_path = project.file_table.resolve(host.file);
    auto borrow = [&] {
        // The chain decides the host as a synthesized context's text does:
        // a change of a file on it may take the header elsewhere.
        if(!resolution.synthesized) {
            for(auto file: host.chain) {
                if(file == path_id) {
                    continue;
                }
                if(auto dep = current_dep(project.file_table, file)) {
                    resolution.embedded.push_back(*dep);
                }
            }
        }
        resolution.host = host.file;
        resolution.occurrence = search.occurrence;
        resolution.forced = host.chain.front() != host.file ? host.chain.front() : Fid{};
        LOG_INFO("resolve_command: {} borrows the command of {} (synthesized={})",
                 path,
                 host_path,
                 resolution.synthesized != nullptr);
        return ref;
    };
    // A header the command forces in itself has nothing to cut: the
    // -includes ahead of it are its context.
    if(host.chain.size() == 1) {
        return borrow();
    }

    // What the header borrows is the host's command whatever a run adds.
    auto lent = *host_command(project, path_id, host.file, pinned_hash, pinned_base);
    // A host the user chose is chosen for its preprocessor state, so the
    // header always compiles in its context.
    std::optional<TrialKey> trial;
    std::optional<bool> alone = false;
    if(!pinned) {
        trial = TrialKey{path_id, project.commands.command_hash(lent.command)};
        alone = self_contained(project, *trial, request.open);
    }
    if(alone == false) {
        auto context = synthesize(project, path_id, host, lent);
        if(!context.result.synthesized) {
            resolution.unmatched_host = host.file;
            return std::nullopt;
        }
        resolution.synthesized = std::move(context.result.synthesized);
        resolution.embedded = std::move(context.trace);
    }
    if(trial) {
        resolution.trial = trial;
        resolution.self_contained = alone;
    }
    return borrow();
}

Resolution CommandResolver::resolve_command(Fid path_id,
                                            std::string& directory,
                                            std::vector<std::string>& arguments,
                                            const CommandRequest& request) {
    auto resolution = resolve(path_id, directory, arguments, request);
    llvm::SmallVector<UnitCommand, 2> taken;
    while(request.preprocess && resolution.missing_tree &&
          !ranges::contains(taken, resolution.missing_tree->key)) {
        taken.push_back(resolution.missing_tree->key);
        take_tree(project, *resolution.missing_tree);
        resolution = resolve(path_id, directory, arguments, request);
    }
    return resolution;
}

Resolution CommandResolver::resolve(Fid path_id,
                                    std::string& directory,
                                    std::vector<std::string>& arguments,
                                    const CommandRequest& request) {
    auto path = project.file_table.resolve(path_id);
    llvm::SmallVector<llvm::StringRef, 4> tried;
    Resolution resolution;

    auto finish = [&](const CommandRef& ref) {
        // A header borrowing its host's command compiles as the host's
        // language, with the header injected as the input; the synthesized
        // prefix lands after the host's own user-content flags (its
        // -includes run first, but for the one the context enters the
        // header through and those after it).
        RenderOptions options;
        if(resolution.synthesized) {
            options.preamble = resolution.synthesized->prefix.c_str();
        }
        options.kept_includes = includes_before(project, ref, resolution.forced);
        directory = project.commands.get(ref.command).directory;
        auto rendered = project.toolchain.render(ref, project.cdb.spelling(ref.file), options);
        arguments.assign(rendered.argv.begin(), rendered.argv.end());
        // The builtin command names a compiler no build chose: clang's own
        // reading of it, when that compiler is missing, loses nothing.
        if(ref.source != CommandSource::Fallback) {
            resolution.query_failure = std::move(rendered.query_failure);
        }
        resolution.hidden = std::move(rendered.hidden);
        resolution.ref = ref;
        resolution.source = ref.source;
        log_command_decision(path, tried, ref.source, arguments);
        return std::move(resolution);
    };

    // `base` with the rule edits of `paths` and the run's extras applied:
    // an entry, a default command, a borrowed or the builtin one alike.
    auto finish_own = [&](CommandID base,
                          CommandSource source,
                          llvm::ArrayRef<CanonicalRef> paths,
                          llvm::StringRef language_path) {
        return finish(project.build.resolve(path_id,
                                            base,
                                            source,
                                            paths,
                                            language_path,
                                            request.extra_prepend,
                                            request.extra_append));
    };

    const Selection* choice = request.selection;
    bool has_host_choice = choice && choice->host_path_id.valid();

    // 1. If the file has an active header context via switchContext, use the
    //    host source's CDB entry with file path replaced and preamble injected.
    if(has_host_choice) {
        tried.push_back("switch_context");
        if(auto ref = resolve_through_host(path_id, request, resolution)) {
            return finish(*ref);
        }
    }

    // 2. The file's own command: a database entry, the default command of
    //    a source the build compiles as a unit, or the command a
    //    provisional member borrows. A header's default command is only
    //    the last resort below, after host inference.
    //    Multi-config projects honor the user's chosen entry, matched by
    //    command hash so the choice survives reordering.
    tried.push_back("cdb");
    auto commands = project.build.commands(path_id);
    if(project.build.unit(path_id)) {
        llvm::StringRef pinned_hash, pinned_base;
        if(choice && !has_host_choice) {
            pinned_hash = choice->command_hash;
            pinned_base = choice->base_hash;
        }
        auto picked =
            pick_pinned_command(project, path_id, commands, path, path, pinned_hash, pinned_base);
        return finish_own(picked.command, picked.source, path, path);
    }

    // 3. No CDB entry — try automatic header context resolution.
    if(!has_host_choice) {
        tried.push_back("include_graph");
        if(auto ref = resolve_through_host(path_id, request, resolution)) {
            return finish(*ref);
        }
    }

    // Until the host's include tree the search waits on is taken, the
    // header stands on that host's command, without its includer's
    // context: a compile takes the tree and resolves again.
    if(auto& missing = resolution.missing_tree) {
        auto host = missing->key.first;
        bool pinned = has_host_choice && choice->host_path_id == host;
        if(auto ref = host_command(project,
                                   path_id,
                                   host,
                                   pinned ? llvm::StringRef(choice->command_hash) : "",
                                   pinned ? llvm::StringRef(choice->base_hash) : "",
                                   request.extra_prepend,
                                   request.extra_append)) {
            return finish(*ref);
        }
    }

    // 4. A rule's default command for a file the build does not compile as
    //    a unit (a header under a default-command rule).
    if(!commands.empty()) {
        tried.push_back("default");
        return finish_own(commands.front().command, CommandSource::Default, path, path);
    }

    // 5. A nearby unit's command, edited for both files as a provisional
    //    member's is. A header compiles in that unit's language — a `.h`
    //    as C++, not as a header job; a source in its own, which lending
    //    only ever matches or crosses between C++ and its modules.
    tried.push_back("inferred");
    if(auto lender = command_lender(project, path_id)) {
        auto lender_path = project.file_table.resolve(lender->unit);
        LOG_INFO("resolve_command: {} borrows the command of {}", path, lender_path);
        resolution.lender = lender->unit;
        return finish_own(project.build.lend(lender->command, lender_path, path),
                          CommandSource::Inferred,
                          path,
                          is_header_path(path) ? lender_path : path);
    }

    // 6. The builtin fallback, so the file still compiles and produces
    //    diagnostics instead of failing silently.
    tried.push_back("fallback");
    return finish_own(project.build.builtin(path), CommandSource::Fallback, path, path);
}

std::optional<CommandRef> CommandResolver::pch_unit(Fid path_id,
                                                    const Resolution& resolution,
                                                    const CommandRequest& request) {
    llvm::SmallVector<CanonicalRef, 2> paths{project.file_table.resolve(path_id)};
    if(resolution.host.valid()) {
        paths.push_back(project.file_table.resolve(resolution.host));
    }
    return project.build.pch_unit(resolution.ref.command,
                                  paths,
                                  request.extra_prepend,
                                  request.extra_append);
}

}  // namespace clice

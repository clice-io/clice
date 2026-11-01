module clice;

import :command.argument;
import :command.search_config;
import :project.command;
import :project.project;
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
/// (SelectedCommand::forced). All of them for none.
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

/// One selection as its tiers run: each tier answers the candidate the
/// file compiles under, or none to pass on to the next.
struct Tiers {
    Project& project;
    Fid file;
    CanonicalRef path;
    const CommandRequest& request;
    SelectedCommand& selection;

    /// The user's host choice; null for none.
    const ContextChoice* host_choice() const {
        auto* choice = request.choice;
        return choice && choice->host.valid() ? choice : nullptr;
    }

    /// A header's command from the host `search` found, in the includer's
    /// context when the header needs it; the host's command alone while the
    /// include tree the search needs is not taken. None when no host stands
    /// in for it.
    std::optional<Candidate> through_host(HostSearch search) {
        auto host = search.host      ? search.host->file
                    : search.missing ? search.missing->key.first
                                     : Fid{};
        if(!host.valid()) {
            return std::nullopt;
        }
        auto* choice = host_choice();
        bool chosen = choice && choice->host == host;
        // The host search ranks only hosts with a command fitting the header.
        auto offered = project.candidates(file, host);
        auto picked =
            chosen ? project.chosen_candidate(offered, choice->base).value_or(offered.front())
                   : offered.front();
        auto candidate = picked.passed_on(host, CommandSource::Host);
        selection.host = host;
        // Until the host's include tree the search waits on is taken, the
        // header stands on that host's command, without its includer's
        // context: a compile takes the tree and resolves again.
        if(search.missing) {
            selection.missing_tree = std::move(search.missing);
            return candidate;
        }
        auto& entry = *search.host;
        selection.occurrence = search.occurrence;
        selection.forced = entry.chain.front() != entry.file ? entry.chain.front() : Fid{};
        LOG_INFO("resolve_command: {} compiles in {}", path, project.file_table.resolve(host));
        // A header the command forces in itself has nothing to cut: the
        // -includes ahead of it are its context. A host the user chose is
        // chosen for its preprocessor state, so the header always compiles in
        // its context; else the self-containment check decides.
        if(entry.chain.size() > 1) {
            if(chosen) {
                selection.in_context = true;
            } else {
                // What the header compiles under is the host's command
                // whatever a run adds.
                auto command = project.apply_rules(file, candidate).command;
                auto key = SelfContainmentKey{file, project.commands.command_hash(command)};
                selection.self_containment = key;
                selection.self_contained = project.self_contained(key, request.open);
                selection.in_context = selection.self_contained == false;
            }
        }
        selection.entry = std::move(entry);
        return candidate;
    }

    /// 1. The host the user chose (clice/switchContext), while its compile
    ///    enters the file where the choice says.
    std::optional<Candidate> chosen_host() {
        auto* choice = host_choice();
        if(!choice) {
            return std::nullopt;
        }
        return through_host(project.find_chosen_host(file, *choice));
    }

    /// 2. The file's own command: a database entry, the default command of
    ///    a source the build compiles as a unit, or a provisional member's
    ///    command. A header's default command is only the last resort
    ///    below, after its hosts. Multi-config projects honor the user's
    ///    chosen entry, matched by its command as written so the choice
    ///    survives reordering.
    std::optional<Candidate> own() {
        if(!project.is_unit(file)) {
            return std::nullopt;
        }
        auto candidates = project.candidates(file);
        auto* choice = host_choice() ? nullptr : request.choice;
        if(!choice) {
            return candidates.front();
        }
        return project.chosen_candidate(candidates, choice->base).value_or(candidates.front());
    }

    /// 3. The host that ranks first for the file, a header without a
    ///    command of its own.
    std::optional<Candidate> ranked_host() {
        return through_host(project.find_ranked_host(file));
    }

    /// 4. A rule's default command for a file the build does not compile
    ///    as a unit (a header under a default-command rule).
    std::optional<Candidate> default_command() {
        if(auto id = project.default_command(path)) {
            return Candidate{.base = *id, .source = CommandSource::Default};
        }
        return std::nullopt;
    }

    /// 5. A nearby member's command, edited for both files as a
    ///    provisional member's is.
    std::optional<Candidate> borrowed() {
        auto borrowed = project.borrowed_command(file, true);
        if(borrowed) {
            LOG_INFO("resolve_command: {} borrows the command of {}",
                     path,
                     project.file_table.resolve(borrowed->from()));
        }
        return borrowed;
    }

    /// 6. The builtin fallback, so the file still compiles and produces
    ///    diagnostics instead of failing silently.
    std::optional<Candidate> fallback() {
        return Candidate{.base = project.fallback_command(path), .source = CommandSource::Fallback};
    }
};

}  // namespace

std::optional<Candidate> Project::chosen_candidate(llvm::ArrayRef<Candidate> candidates,
                                                   llvm::StringRef base) {
    auto it = ranges::find_if(candidates, [&](const Candidate& candidate) {
        return commands.command_hash_hex(candidate.base) == base;
    });
    return it != candidates.end() ? std::optional(*it) : std::nullopt;
}

SelectedCommand Project::select_command(Fid path_id, const CommandRequest& request) {
    auto path = file_table.resolve(path_id);
    SelectedCommand selection;
    Tiers tiers{*this, path_id, path, request, selection};
    constexpr std::pair<llvm::StringRef, std::optional<Candidate> (Tiers::*)()> order[] = {
        {"chosen_host", &Tiers::chosen_host    },
        {"own",         &Tiers::own            },
        {"ranked_host", &Tiers::ranked_host    },
        {"default",     &Tiers::default_command},
        {"borrowed",    &Tiers::borrowed       },
        {"fallback",    &Tiers::fallback       },
    };
    std::optional<Candidate> candidate;
    for(auto [name, tier]: order) {
        selection.tried.push_back(name);
        if((candidate = (tiers.*tier)())) {
            break;
        }
    }
    selection.candidate = *candidate;
    selection.ref = apply_rules(path_id, *candidate, request.extra_prepend, request.extra_append);
    return selection;
}

Rendered Project::render(const CommandRef& ref, const RenderOptions& options) {
    Rendered result{.directory = commands.get(ref.command).directory};
    auto rendered = toolchain.render(ref, cdb.spelling(ref.file), options);
    result.arguments.assign(rendered.argv.begin(), rendered.argv.end());
    result.hidden = std::move(rendered.hidden);
    // The builtin command names a compiler no build chose: clang's own
    // reading of it, when that compiler is missing, loses nothing.
    if(ref.source != CommandSource::Fallback) {
        result.query_failure = std::move(rendered.query_failure);
    }
    return result;
}

Resolution Project::resolve_command(Fid path_id, const CommandRequest& request) {
    Resolution resolution;
    llvm::SmallVector<UnitCommand, 2> taken;
    while(true) {
        static_cast<SelectedCommand&>(resolution) = select_command(path_id, request);
        if(!request.preprocess || !resolution.missing_tree ||
           ranges::contains(taken, resolution.missing_tree->key)) {
            break;
        }
        taken.push_back(resolution.missing_tree->key);
        take_tree(*resolution.missing_tree);
    }

    // The header's context: the includer's state up to where the host's
    // compile enters it, synthesized when the header needs it, else the
    // files of the chain alone, which decide the host as a synthesized
    // context's text does — a change of one may take the header elsewhere.
    if(auto& entry = resolution.entry) {
        if(resolution.in_context) {
            auto command = apply_rules(path_id, resolution.candidate);
            auto context = synthesize(path_id, *entry, command);
            if(context.result.synthesized) {
                resolution.synthesized = std::move(context.result.synthesized);
                resolution.inputs = std::move(context.inputs);
            } else {
                resolution.unmatched_host = entry->file;
            }
        }
        if(!resolution.synthesized) {
            for(auto chained: entry->chain) {
                if(chained == path_id) {
                    continue;
                }
                if(auto input = current_input(file_table, chained)) {
                    resolution.inputs.push_back(*input);
                }
            }
        }
    }

    // A header compiling in its host compiles as the host's language, with
    // the header injected as the input; the synthesized prefix lands after
    // the host's own user-content flags (its -includes run first, but for
    // the one the context enters the header through and those after it).
    RenderOptions options;
    if(resolution.synthesized) {
        options.preamble = resolution.synthesized->prefix.c_str();
    }
    options.kept_includes = includes_before(*this, resolution.ref, resolution.forced);
    static_cast<Rendered&>(resolution) = render(resolution.ref, options);
    log_command_decision(file_table.resolve(path_id),
                         resolution.tried,
                         resolution.ref.source,
                         resolution.arguments);
    return resolution;
}

const clang::LangOptions& Project::dialect(Fid file, const ContextChoice* choice) {
    auto ref = select_command(file, {.choice = choice}).ref;
    auto language = language_of(ref);
    bool is_c =
        language == clang::driver::types::TY_C || language == clang::driver::types::TY_CHeader;
    llvm::StringRef standard;
    for(auto& arg: commands.get(ref.command).args) {
        if(arg.opt_id == clang_options::OPT_std_EQ && arg.values.size() == 1) {
            standard = arg.values[0];
        }
    }
    return dialect_of("", is_c, standard);
}

std::uint64_t Project::command_identity(Fid unit) {
    auto candidates = this->candidates(unit);
    if(candidates.empty()) {
        return 0;
    }
    auto& candidate = candidates.front();
    std::uint64_t parts[] = {commands.command_hash(candidate.base),
                             rules.identity(rule_edits(unit, candidate))};
    return llvm::xxh3_64bits(llvm::StringRef(reinterpret_cast<const char*>(parts), sizeof(parts)));
}

std::optional<CommandRef> Project::pch_writer(Fid file,
                                              const SelectedCommand& selection,
                                              const CommandRequest& request) {
    auto command = selection.ref.command;
    if(commands.pch_output(command)) {
        return std::nullopt;
    }
    for(auto& input: commands.pch_inputs(command)) {
        auto pch = file_table.find(input);
        if(!pch) {
            continue;
        }
        for(auto& writer: cdb.pch_writers(*pch)) {
            bool active = ranges::any_of(entries(writer.file), [&](const CompilationEntry& entry) {
                return entry.source == writer.source && entry.ordinal == writer.ordinal;
            });
            if(active) {
                auto applied = edited_commands.apply(
                    writer.command,
                    rule_edits(file, selection.candidate)
                        .options(request.extra_prepend, request.extra_append));
                return CommandRef{writer.file,
                                  applied,
                                  commands.input_kind(applied, file_table.resolve(writer.file)),
                                  CommandSource::Database};
            }
        }
    }
    return std::nullopt;
}

}  // namespace clice

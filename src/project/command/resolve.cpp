module clice;

import :command.argument;
import :command.file_kind;
import :command.search_config;
import :index.project_index;
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

/// What a command compiles its file as: whether as C, whether an -x
/// forces that (the driver override beats every suffix heuristic), and the
/// last -std value.
struct Language {
    bool is_c = false;
    bool language_forced = false;
    std::string standard;
};

Language language_facts(Project& project, const CommandRef& ref) {
    auto language = language_of(ref);
    Language facts{
        .is_c =
            language == clang::driver::types::TY_C || language == clang::driver::types::TY_CHeader,
        .language_forced = !project.commands.forced_language(ref.command).empty(),
    };
    for(auto& arg: project.commands.get(ref.command).args) {
        if(arg.opt_id == clang_options::OPT_std_EQ && arg.values.size() == 1) {
            facts.standard = arg.values[0];
        }
    }
    return facts;
}

/// One resolution as its tiers run: each tier answers the candidate the
/// file compiles under, or none to pass on to the next.
struct Tiers {
    Project& project;
    Fid file;
    CanonicalRef path;
    const CommandRequest& request;
    Resolution& resolution;

    /// The user's host choice; null for none.
    const ContextChoice* host_choice() const {
        auto* choice = request.choice;
        return choice && choice->host_path_id.valid() ? choice : nullptr;
    }

    /// A header's command lent by the host `search` found, with the
    /// includer's context when the header needs it and it can be
    /// synthesized; the host's command alone while the include tree the
    /// search needs is not taken, or the context cannot be followed. None
    /// when no host stands in for it.
    std::optional<Candidate> through_host(HostSearch search) {
        auto host_file = search.host      ? search.host->file
                         : search.missing ? search.missing->key.first
                                          : Fid{};
        if(!host_file.valid()) {
            return std::nullopt;
        }
        auto* choice = host_choice();
        bool chosen = choice && choice->host_path_id == host_file;
        // The host search ranks only hosts with a command fitting the header.
        auto picked =
            *project.chosen_host_command(file,
                                         host_file,
                                         chosen ? llvm::StringRef(choice->command_hash) : "",
                                         chosen ? llvm::StringRef(choice->base_hash) : "");
        auto candidate = picked.passed_on(host_file, CommandSource::Host);
        resolution.host = host_file;
        // Until the host's include tree the search waits on is taken, the
        // header stands on that host's command, without its includer's
        // context: a compile takes the tree and resolves again.
        if(search.missing) {
            resolution.missing_tree = std::move(search.missing);
            return candidate;
        }
        auto& host = *search.host;
        resolution.occurrence = search.occurrence;
        resolution.forced = host.chain.front() != host.file ? host.chain.front() : Fid{};
        LOG_INFO("resolve_command: {} borrows the command of {}",
                 path,
                 project.file_table.resolve(host.file));
        // A header the command forces in itself has nothing to cut: the
        // -includes ahead of it are its context.
        if(host.chain.size() > 1) {
            in_context(host, candidate, chosen);
        }
        // The chain decides the host as a synthesized context's text does:
        // a change of a file on it may take the header elsewhere.
        if(!resolution.synthesized) {
            for(auto chained: host.chain) {
                if(chained == file) {
                    continue;
                }
                if(auto dep = current_input(project.file_table, chained)) {
                    resolution.inputs.push_back(*dep);
                }
            }
        }
        return candidate;
    }

    /// Whether the header compiles in its includer's context under
    /// `candidate`, and the context when it does. A host the user chose is
    /// chosen for its preprocessor state, so the header always compiles in
    /// its context; one that cannot be followed leaves the header the
    /// host's command alone (Resolution::unmatched_host).
    void in_context(const Host& host, const Candidate& candidate, bool chosen) {
        // What the header borrows is the host's command whatever a run adds.
        auto lent = project.apply_rules(file, candidate);
        std::optional<bool> alone = false;
        if(!chosen) {
            auto key = SelfContainmentKey{file, project.commands.command_hash(lent.command)};
            alone = project.self_contained(key, request.open);
            resolution.self_containment = key;
            resolution.self_contained = alone;
        }
        if(alone != false) {
            return;
        }
        auto context = project.synthesize(file, host, lent);
        if(!context.result.synthesized) {
            resolution.unmatched_host = host.file;
            return;
        }
        resolution.synthesized = std::move(context.result.synthesized);
        resolution.inputs = std::move(context.inputs);
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
    ///    below, after host inference. Multi-config projects honor the
    ///    user's chosen entry, matched by command hash so the choice
    ///    survives reordering.
    std::optional<Candidate> own() {
        if(!project.is_unit(file)) {
            return std::nullopt;
        }
        auto commands = project.candidates(file);
        auto* choice = host_choice() ? nullptr : request.choice;
        return project
            .chosen_candidate(file,
                              commands,
                              {},
                              choice ? llvm::StringRef(choice->command_hash) : "",
                              choice ? llvm::StringRef(choice->base_hash) : "")
            .value_or(commands.front());
    }

    /// 3. The host that ranks first for the file, a header without a
    ///    command of its own.
    std::optional<Candidate> ranked_host() {
        return through_host(project.find_ranked_host(file));
    }

    /// 4. A rule's default command for a file the build does not compile
    ///    as a unit (a header under a default-command rule).
    std::optional<Candidate> default_command() {
        auto commands = project.candidates(file);
        if(commands.empty()) {
            return std::nullopt;
        }
        return Candidate{.base = commands.front().base, .source = CommandSource::Default};
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

/// resolve_command, the trees the host search misses left missing.
Resolution resolve(Project& project,
                   Fid path_id,
                   std::string& directory,
                   std::vector<std::string>& arguments,
                   const CommandRequest& request) {
    auto path = project.file_table.resolve(path_id);
    Resolution resolution;
    Tiers tiers{project, path_id, path, request, resolution};
    constexpr std::pair<llvm::StringRef, std::optional<Candidate> (Tiers::*)()> order[] = {
        {"chosen_host", &Tiers::chosen_host    },
        {"own",         &Tiers::own            },
        {"ranked_host", &Tiers::ranked_host    },
        {"default",     &Tiers::default_command},
        {"borrowed",    &Tiers::borrowed       },
        {"fallback",    &Tiers::fallback       },
    };
    llvm::SmallVector<llvm::StringRef, 6> tried;
    std::optional<Candidate> candidate;
    for(auto [name, tier]: order) {
        tried.push_back(name);
        if((candidate = (tiers.*tier)())) {
            break;
        }
    }

    auto ref =
        project.apply_rules(path_id, *candidate, request.extra_prepend, request.extra_append);
    // A header borrowing its host's command compiles as the host's
    // language, with the header injected as the input; the synthesized
    // prefix lands after the host's own user-content flags (its -includes
    // run first, but for the one the context enters the header through and
    // those after it).
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
    resolution.candidate = *candidate;
    resolution.ref = ref;
    log_command_decision(path, tried, ref.source, arguments);
    return resolution;
}

}  // namespace

std::optional<Candidate> Project::chosen_candidate(Fid file,
                                                   llvm::ArrayRef<Candidate> candidates,
                                                   Fid host,
                                                   llvm::StringRef chosen_hash,
                                                   llvm::StringRef chosen_base) {
    // The base identity resolved when the user chose is exact; the applied hash
    // remains as the fallback for choices saved before the base was recorded
    // (and cannot distinguish candidates the rules collapse together).
    if(!chosen_base.empty()) {
        for(auto& entry: candidates) {
            if(commands.command_hash_hex(entry.base) == chosen_base) {
                return entry;
            }
        }
    }
    if(!chosen_hash.empty()) {
        for(auto& entry: candidates) {
            auto ref =
                apply_rules(file,
                            host.valid() ? entry.passed_on(host, CommandSource::Host) : entry);
            if(commands.command_hash_hex(ref.command) == chosen_hash) {
                return entry;
            }
        }
    }
    return std::nullopt;
}

CommandRef Project::lent_command(Fid header,
                                 Fid host,
                                 const Candidate& command,
                                 llvm::ArrayRef<std::string> extra_prepend,
                                 llvm::ArrayRef<std::string> extra_append) {
    return apply_rules(header,
                       command.passed_on(host, CommandSource::Host),
                       extra_prepend,
                       extra_append);
}

std::optional<Candidate> Project::chosen_host_command(Fid header,
                                                      Fid host,
                                                      llvm::StringRef chosen_hash,
                                                      llvm::StringRef chosen_base) {
    auto commands = host_commands(header, host);
    if(commands.empty()) {
        return std::nullopt;
    }
    return chosen_candidate(header, commands, host, chosen_hash, chosen_base)
        .value_or(commands.front());
}

std::optional<CommandRef> Project::host_command(Fid header,
                                                Fid host,
                                                llvm::StringRef chosen_hash,
                                                llvm::StringRef chosen_base,
                                                llvm::ArrayRef<std::string> extra_prepend,
                                                llvm::ArrayRef<std::string> extra_append) {
    auto picked = chosen_host_command(header, host, chosen_hash, chosen_base);
    if(!picked) {
        return std::nullopt;
    }
    return lent_command(header, host, *picked, extra_prepend, extra_append);
}

const clang::LangOptions& Project::dialect(Fid file, const ContextChoice* choice, Fid host) {
    auto path = file_table.resolve(file);
    bool host_choice = choice && choice->host_path_id.valid();
    std::optional<CommandRef> own;
    if(auto commands = candidates(file); !commands.empty()) {
        auto* own_choice = host_choice ? nullptr : choice;
        auto candidate =
            chosen_candidate(file,
                             commands,
                             {},
                             own_choice ? llvm::StringRef(own_choice->command_hash) : "",
                             own_choice ? llvm::StringRef(own_choice->base_hash) : "")
                .value_or(commands.front());
        own = apply_rules(file, candidate);
    }
    auto own_facts = own ? std::optional(language_facts(*this, *own)) : std::nullopt;
    // A file's entry is its command; a default command yields to the host
    // a header borrows from, in resolve_command's order.
    if(own_facts && own_facts->language_forced && own->source == CommandSource::Database) {
        return dialect_of("", own_facts->is_c, own_facts->standard);
    }

    // A header's active context (the user's persisted choice, else the
    // resolved host) names the view being read; its command beats the
    // contributor union the way it does for the AST after an escalation.
    if(host.valid()) {
        bool chosen = host_choice && choice->host_path_id == host;
        if(auto lent = host_command(file,
                                    host,
                                    chosen ? llvm::StringRef(choice->command_hash) : "",
                                    chosen ? llvm::StringRef(choice->base_hash) : "")) {
            auto facts = language_facts(*this, *lent);
            return dialect_of("", facts.is_c, facts.standard);
        }
    }

    if(own_facts && own_facts->language_forced) {
        return dialect_of("", own_facts->is_c, own_facts->standard);
    }

    bool contributed = false;
    bool c_only = true;
    index.for_each_contribution(file, [&](Fid tu, std::uint64_t) {
        contributed = true;
        c_only = c_only && llvm::StringRef(file_table.resolve(tu)).ends_with(".c");
    });
    c_only = contributed && c_only;
    return dialect_of(path,
                      c_only,
                      own_facts ? llvm::StringRef(own_facts->standard) : llvm::StringRef());
}

Resolution Project::resolve_command(Fid path_id,
                                    std::string& directory,
                                    std::vector<std::string>& arguments,
                                    const CommandRequest& request) {
    auto resolution = resolve(*this, path_id, directory, arguments, request);
    llvm::SmallVector<UnitCommand, 2> taken;
    while(request.preprocess && resolution.missing_tree &&
          !ranges::contains(taken, resolution.missing_tree->key)) {
        taken.push_back(resolution.missing_tree->key);
        take_tree(*resolution.missing_tree);
        resolution = resolve(*this, path_id, directory, arguments, request);
    }
    return resolution;
}

std::uint64_t Project::command_identity(Fid unit) {
    auto commands = candidates(unit);
    if(commands.empty()) {
        return 0;
    }
    auto& candidate = commands.front();
    std::uint64_t parts[] = {this->commands.command_hash(candidate.base),
                             rules.identity(rule_edits(unit, candidate))};
    return llvm::xxh3_64bits(llvm::StringRef(reinterpret_cast<const char*>(parts), sizeof(parts)));
}

std::optional<CommandRef> Project::pch_writer(Fid path_id,
                                              const Resolution& resolution,
                                              const CommandRequest& request) {
    return pch_writer(path_id,
                      resolution.candidate,
                      resolution.ref.command,
                      request.extra_prepend,
                      request.extra_append);
}

}  // namespace clice

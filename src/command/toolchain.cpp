module clice;

import :command.argument;
import :command.edit;
import :command.search_config;
import :command.table;
import :command.toolchain;
import :command.toolchain_query;
import :command.toolchains.clang;
import :command.toolchains.nvcc;
import :support.failure;
import :support.logging;
import :support.process;
import :support.text;
import :vfs.dir_cache;
import :vfs.file_system;
import :vfs.path;

namespace clice {

namespace {

/// The query working directory, empty when the wanted one does not exist
/// (a stale CDB directory, or a foreign-platform path): querying from the
/// process cwd degrades the answer instead of failing the spawn.
std::string query_cwd(llvm::StringRef wanted) {
    if(wanted.empty() || !fs::is_directory(wanted)) {
        return {};
    }
    return wanted.str();
}

/// Every component of a search-path-style environment value is absolute and
/// non-empty (an empty component means the cwd).
bool components_all_absolute(llvm::StringRef value) {
    return ranges::all_of(sys::split_paths(value), [](std::string_view part) {
        return !part.empty() && path::is_absolute(part);
    });
}

/// Whether every search-path environment variable the compiler consults is
/// absolute — one of the cwd-insensitivity conditions. Computed once: the
/// server never mutates its environment.
bool search_env_all_absolute() {
    const static bool result = [] {
#ifdef _WIN32
        for(const char* name: {"INCLUDE", "LIB", "PATH"}) {
            if(auto value = env_string(name)) {
                if(!components_all_absolute(*value))
                    return false;
            }
        }
#else
        for(const char* name: {"PATH",
                               "CPATH",
                               "C_INCLUDE_PATH",
                               "CPLUS_INCLUDE_PATH",
                               "OBJC_INCLUDE_PATH",
                               "COMPILER_PATH",
                               "LIBRARY_PATH"}) {
            if(auto value = env_string(name)) {
                if(!components_all_absolute(*value))
                    return false;
            }
        }
        /// A single path, not a list.
        if(auto prefix = env_string("GCC_EXEC_PREFIX")) {
            if(!path::is_absolute(*prefix))
                return false;
        }
#endif
        return true;
    }();
    return result;
}

/// Whether an option clang does not know may name a path relative to the
/// working directory: nothing tells, so any relative-looking value does.
bool relative_suspect(llvm::StringRef value) {
    if(value.empty() || path::is_absolute(value)) {
        return false;
    }
    return value.contains('/') || value.contains('\\') || value.starts_with(".");
}

bool names_search_dir(unsigned id) {
    return id == clang_options::OPT_I || id == clang_options::OPT_isystem ||
           id == clang_options::OPT_idirafter || id == clang_options::OPT_internal_isystem ||
           id == clang_options::OPT_internal_externc_isystem;
}

/// Whether `dir` holds a clang's builtin headers.
bool holds_builtin_headers(llvm::StringRef dir) {
    return fs::exists(path::join(dir, "__stddef_max_align_t.h"));
}

/// The files under `dir`, by their paths relative to it, `/`-separated.
std::vector<std::string> files_under(const Spelling& dir) {
    std::vector<std::string> names;
    for(auto& entry: fs::walk(dir)) {
        if(!entry) {
            vfs::warn_unreadable(entry.error());
        } else if(entry->type != fs::FileType::Directory) {
            names.emplace_back(entry->relative());
        }
    }
    return names;
}

/// Whether the frontend takes `arg` as the command writes it, the driver
/// passing it on untouched to cc1, so the query need not see it: clang's
/// `-W`, `-w` and `-pedantic`, and `-fmodule-name=`, which outside header
/// modules (normalize drops it with them) names only `__MODULE__`. Targets
/// setting their warnings or their module names apart would each ask the
/// compiler (Chromium names every target's module). One a driver translates
/// (cl's `/W4`, `-Wframe-larger-than=`) the query sees, as it does every
/// option of a compiler whose flags clang takes only after flags_for_clang
/// (GCC's `-Werror=shadow`).
bool frontend_takes_as_written(const Arg& arg, CompilerFamily family) {
    if(cl_mode(family) || family == CompilerFamily::GCC || family == CompilerFamily::NVCC) {
        return false;
    }
    if(arg.cls == ArgClass::Semantic) {
        return arg.opt_id == clang_options::OPT_fmodule_name_EQ;
    }
    auto opt = clang_options::table().option(arg.opt_id);
    return arg.cls == ArgClass::Diagnostics && opt &&
           opt->has_visibility_flag(clang_options::CC1Option);
}

/// Which args feed the query (and its key): everything that may change
/// driver behavior. User-content never does, nor does the command's
/// action, nor what the frontend takes as written; unknown tokens feed it
/// only as NVCC query tokens (junk from the CDB must not fail an otherwise
/// good query).
bool in_query_view(const Arg& arg, CompilerFamily family) {
    switch(arg.cls) {
        case ArgClass::Semantic: {
            // The query picks its own action; the command's (-fsyntax-only)
            // would go unused, an error under -Werror.
            auto opt = clang_options::table().option(arg.opt_id);
            return (!opt || !opt->matches(clang_options::OPT_Action_Group)) &&
                   !frontend_takes_as_written(arg, family);
        }
        case ArgClass::Diagnostics: return !frontend_takes_as_written(arg, family);
        case ArgClass::Unknown:
            return family == CompilerFamily::NVCC && is_nvcc_query_flag(arg.spelling);
        case ArgClass::UserContent:
        case ArgClass::Precompiled:
        case ArgClass::Input: return false;
    }
    std::unreachable();
}

/// Drops the SYCL integration header and footer an Intel host job forces
/// in: its device job writes them to a temporary directory, gone once the
/// query ends. They are the forced includes the job keeps out of its
/// dependencies (`-dependency-filter <file>`). oneAPI includes them with
/// `-include-internal-header` and `-include-internal-footer`, before
/// 2025.1 with `-include` and `-include-footer`; clang's own options read
/// all but `-include` as an `-include` of a file named by the flag's rest.
void drop_sycl_integration_files(std::vector<std::string>& tokens) {
    llvm::SmallVector<std::string> generated;
    for(std::size_t i = 0; i + 1 < tokens.size(); i += 1) {
        if(tokens[i] == "-dependency-filter") {
            generated.push_back(tokens[i + 1]);
        }
    }
    constexpr std::array<llvm::StringRef, 4> includes = {"-include",
                                                         "-include-footer",
                                                         "-include-internal-header",
                                                         "-include-internal-footer"};
    for(std::size_t i = 0; i + 1 < tokens.size();) {
        if(ranges::contains(includes, llvm::StringRef(tokens[i])) &&
           ranges::contains(generated, tokens[i + 1])) {
            tokens.erase(tokens.begin() + i, tokens.begin() + i + 2);
        } else {
            i += 1;
        }
    }
}

/// Where among a job's arguments the values of the command's -Xclang
/// options start: the driver passes them on verbatim, in one run. The end
/// of `tokens` when the command has none.
std::size_t forwarded_start(llvm::ArrayRef<std::string> tokens, const Command& command) {
    llvm::SmallVector<llvm::StringRef> values;
    for(auto& arg: command.args) {
        if(is_xclang_option(arg.opt_id)) {
            values.push_back(arg.values[0]);
        }
    }
    auto run = ranges::find_end(tokens, values, [](llvm::StringRef a, llvm::StringRef b) {
        return a == b;
    });
    return run.begin() - tokens.begin();
}

}  // namespace

Toolchain::Toolchain(CommandTable& commands,
                     FileTable& files,
                     chrono::steady_clock::duration failed_retry) :
    commands(commands), files(files), query_policy{.cooldown = failed_retry} {
    if(auto ms = env_integer("CLICE_TEST_QUERY_RETRY_MS")) {
        query_policy.cooldown = chrono::milliseconds(*ms);
    }
}

Toolchain::~Toolchain() = default;

Toolchain::QueryKey Toolchain::query_key(CommandID id, InputKind input) {
    auto& command = commands.get(id);
    QueryKey out;

    /// cwd-insensitivity is an exemption earned by four orthogonal
    /// conditions; any miss keys (and runs) the query in the entry
    /// directory. The conditions are independent — an absolute driver does
    /// not excuse a relative CPATH.
    bool known_family = command.family != CompilerFamily::Unknown;

    llvm::StringRef driver = command.driver;
    /// A bare name resolves through PATH alone (whose components the env
    /// condition below covers); a relative path is cwd-bound.
    bool driver_cwd_free =
        path::is_absolute(driver) || (!driver.contains('/') && !driver.contains('\\'));

    /// A known option's path values were anchored when the command was
    /// read (names_path); only an unknown one can still hold a
    /// relative path.
    bool values_clean = true;
    for(auto& arg: command.args) {
        if(arg.opt_id != clang_options::OPT_UNKNOWN || !in_query_view(arg, command.family)) {
            continue;
        }
        llvm::StringRef spelling(arg.spelling);
        if(relative_suspect(spelling.substr(spelling.find('=') + 1))) {
            values_clean = false;
        }
    }

    out.cwd_sensitive =
        !(known_family && driver_cwd_free && search_env_all_absolute() && values_clean);

    auto append = [&](llvm::StringRef fragment) {
        out.key += fragment;
        out.key += '\0';
    };

    append(command.driver);
    if(command.subcommand) {
        append(command.subcommand);
    }
    append(input.value ? input.value : "");
    if(!command.query_toolchain) {
        append("\x01unqueried");
    }
    if(out.cwd_sensitive) {
        append(command.directory);
    }

    for(auto& arg: command.args) {
        if(!in_query_view(arg, command.family)) {
            continue;
        }
        out.key +=
            std::to_string(keeps_alias(arg, option_family(command)) ? arg.alias : arg.opt_id);
        out.key += '\0';
        /// All unknown options share one id; their identity is the spelling
        /// (the NVCC translation carries `-ccbin=<path>` through here, and
        /// two commands differing only in host compiler must not collide).
        if(arg.opt_id == clang_options::OPT_UNKNOWN) {
            append(arg.spelling);
        }
        for(const char* value: arg.values) {
            append(value);
        }
    }

    return out;
}

Toolchain::QueryArgv Toolchain::query_argv(const Command& command) {
    QueryArgv out;
    out.argv.push_back(command.driver);
    if(command.subcommand) {
        out.argv.push_back(command.subcommand);
    }

    auto emit = [&](llvm::StringRef fragment) {
        out.argv.push_back(commands.intern(fragment).data());
    };

    out.slot = out.argv.size();
    // A universal build (`-arch arm64 -arch x86_64`) compiles once for each
    // architecture, and a driver preprocesses for one only: clice reads the
    // code for the first.
    bool arch = false;
    for(auto& arg: command.args) {
        if(arg.cls == ArgClass::Input) {
            out.slot = out.argv.size();
            continue;
        }
        if(!in_query_view(arg, command.family) || (arch && arg.opt_id == clang_options::OPT_arch)) {
            continue;
        }
        arch = arch || arg.opt_id == clang_options::OPT_arch;
        if(arg.opt_id == clang_options::OPT_UNKNOWN) {
            out.argv.push_back(arg.spelling);
            continue;
        }
        render_driver_arg(arg, option_family(command), emit);
    }

    return out;
}

std::optional<std::string> Toolchain::ours_for(unsigned id,
                                               llvm::StringRef value,
                                               llvm::StringRef job_resource_dir) {
    llvm::StringRef rest = value;
    if(!job_resource_dir.empty() && job_resource_dir != resource_dir() &&
       rest.consume_front(job_resource_dir) &&
       (rest.empty() || rest.front() == '/' || rest.front() == '\\')) {
        rest = rest.ltrim("/\\");
        if(rest.empty()) {
            return resource_dir().str();
        }
        if(rest == "include" || rest.starts_with("include/") || rest.starts_with("include\\")) {
            return path::join(resource_dir(), rest);
        }
        return std::nullopt;
    }
    auto builtin_headers = path::join(resource_dir(), "include");
    if(!names_search_dir(id) || value.rtrim("/\\") == builtin_headers) {
        return std::nullopt;
    }
    auto [known, inserted] = builtin_header_dirs.try_emplace(value);
    if(inserted) {
        known->second = holds_builtin_headers(value);
    }
    return known->second ? std::optional(builtin_headers) : std::nullopt;
}

Toolchain::Frontend Toolchain::frontend(const Job& job, const Command& command) {
    std::vector<std::string> tokens(job.args.begin() + 2, job.args.end());
    drop_sycl_integration_files(tokens);
    auto forwarded_from = forwarded_start(tokens, command);
    // An external clang may be newer than the linked one: a flag our cc1
    // does not know parses as unknown, and its values as inputs; both go.
    // LIMITATION: so do Apple clang's own cc1 flags (-clang-vendor-feature=,
    // -fno-odr-hash-protocols), and the macros only its fork predefines
    // (TARGET_OS_ARROW, __ARM_ARCH_8_3__) stay undefined; code testing
    // them is rare, and no difference has been seen. Under Intel's oneAPI
    // 2024.2 a SYCL kernel given as an unnamed lambda is an error, as the
    // cc1 flag of Intel's clang allowing it is not ours, and under 2025.0 a
    // SYCL header's bfloat16 is an incomplete type: one false error each in
    // a SYCL project; 2025.1 and later are clean.
    auto parse_options = cli::ParseOptions{.visibility = clang_options::CC1Option};
    std::vector<cli::ParsedArg> kept;
    llvm::StringRef triple;
    llvm::StringRef job_resource_dir;
    for(auto& parsed: clang_options::table().parse(tokens, parse_options)) {
        if(!parsed || parsed->id == clang_options::OPT_UNKNOWN) {
            continue;
        }
        switch(parsed->id) {
            // The query's own action, input and outputs; the render gives
            // the real file its -main-file-name, and clice manages module
            // outputs itself (the driver derives -fmodule-output=<query>.pcm
            // for a module unit).
            case clang_options::OPT_E:
            case clang_options::OPT_fsyntax_only:
            case clang_options::OPT_dM:
            case clang_options::OPT_v:
            case clang_options::OPT_o:
            case clang_options::OPT_INPUT:
            case clang_options::OPT_main_file_name:
            case clang_options::OPT_fmodule_output:
            case clang_options::OPT_fmodule_output_EQ: continue;
            case clang_options::OPT_triple: triple = parsed->values[0]; break;
            // A trailing separator on the command's -resource-dir reaches
            // cc1 verbatim while the derived paths append without doubling
            // it.
            case clang_options::OPT_resource_dir:
                job_resource_dir = llvm::StringRef(parsed->values[0]).rtrim("/\\");
                break;
        }
        kept.push_back(std::move(*parsed));
    }

    // The target's defaults stand in for the names it does not know.
    TargetNames names(triple);

    Frontend result{.driver = commands.intern(job.args[0]).data(),
                    .job_resource_dir = commands.intern(job_resource_dir)};
    auto builtin_headers = path::join(resource_dir(), "include");
    std::vector<std::string> shadowed = job.shadowed;
    std::vector<LocalArg> staged;
    std::optional<std::size_t> first_forwarded;
    for(auto& arg: kept) {
        if(arg.index >= forwarded_from && !first_forwarded) {
            first_forwarded = staged.size();
        }
        if(!arg.values.empty() && !names.knows(arg.id, arg.values[0])) {
            LOG_INFO("clang's target {} knows no CPU or ABI named {} ({}): its default stands in",
                     triple,
                     arg.values[0],
                     clang_options::table().option(arg.id)->prefixed_name());
            continue;
        }
        // What a command leaves out goes here too: a newer external clang
        // may give a known codegen option a value the linked one cannot
        // parse (-mframe-pointer=non-leaf-no-reserve).
        auto local = commands.local_arg(arg, tokens);
        if(!local) {
            continue;
        }
        auto theirs = *local;
        for(auto& value: local->values) {
            if(auto ours = ours_for(local->opt_id, value, job_resource_dir)) {
                value = commands.intern(*ours).data();
            }
        }
        staged.push_back(std::move(*local));
        // Another clang's builtin headers stay right after ours, which
        // shadow them: what only theirs has is found there.
        if(names_search_dir(theirs.opt_id) &&
           llvm::StringRef(staged.back().values[0]) == builtin_headers &&
           llvm::StringRef(theirs.values[0]) != builtin_headers &&
           fs::is_directory(theirs.values[0])) {
            shadowed.push_back(theirs.values[0]);
            staged.push_back(std::move(theirs));
        }
    }
    staged.push_back({.opt_id = clang_options::OPT_fsyntax_only, .cls = ArgClass::Semantic});
    result.forwarded = first_forwarded.value_or(staged.size());
    result.args = commands.persist(staged);
    if(!shadowed.empty()) {
        auto base = Spelling::cwd();
        std::vector<Spelling> spelled;
        for(auto& dir: shadowed) {
            result.shadowed.push_back(commands.intern(dir));
            spelled.emplace_back(dir, base);
        }
        result.hidden = hidden_files(extract_search_config(result.args, base, files), spelled);
    }
    return result;
}

std::shared_ptr<const vfs::HiddenFiles> Toolchain::hidden_files(const SearchConfig& search,
                                                                llvm::ArrayRef<Spelling> shadowed) {
    vfs::Scope scope(files.dirs);
    std::map<std::string, std::set<std::string>> hidden;
    for(auto& dir: shadowed) {
        // The search list keeps one spelling of a directory the job names
        // twice (by a symlink, through `..`), as clang does: the files of
        // the other are never looked up.
        auto found = ranges::find(search.dirs, dir.str(), &SearchDir::path);
        if(found == search.dirs.end()) {
            continue;
        }
        auto [known, inserted] = shadowed_headers.try_emplace(dir.str());
        auto& headers = known->second;
        if(inserted) {
            for(auto& name: files_under(dir)) {
                if(builtin_header(name) != BuiltinHeader::None) {
                    headers.names.push_back(std::move(name));
                }
            }
        }
        if(headers.names.empty()) {
            continue;
        }
        hidden[dir.str()].insert(headers.names.begin(), headers.names.end());
        // Read off the directories' listings: a clang-cl job searches a
        // dozen directories past its clang's builtin headers, hundreds of
        // them.
        for(auto& later: llvm::make_range(std::next(found), search.dirs.end())) {
            auto& listing = scope.list(later.path);
            for(auto& name: headers.names) {
                // A module map is no header: clang reads the one of every
                // directory it searches, #include_next or not.
                if(llvm::StringRef(name).ends_with(".modulemap") ||
                   !listing.contains(llvm::StringRef(name).split('/').first)) {
                    continue;
                }
                auto file = path::join(later.path, name);
                if(!scope.list(path::parent(file)).contains_file(path::filename(file))) {
                    continue;
                }
                auto [defers, unread] = headers.defers.try_emplace(name);
                if(unread) {
                    auto text = vfs::read_text(path::join(dir, name));
                    defers->second = !text || text->contains("include_next");
                }
                if(!defers->second) {
                    hidden[later.path].insert(name);
                }
            }
        }
    }
    if(hidden.empty()) {
        return nullptr;
    }
    auto result = std::make_shared<vfs::HiddenFiles>();
    for(auto& [dir, names]: hidden) {
        result->dirs.push_back({
            .path = dir,
            .names = {names.begin(), names.end()}
        });
    }
    return result;
}

Toolchain::ResolvedID Toolchain::synthesize(CommandID id, const Frontend& query) {
    auto& command = commands.get(id);

    /// The PCM clice builds is the BMI the importers of the real build read:
    /// a reduced one drops what the global module fragment declares and the
    /// purview never names, partial specializations among them. --precompile
    /// writes a full BMI to -o whatever the driver asks for, and a reduced
    /// one only to a module output the command names.
    auto has = [&](unsigned id) {
        return ranges::any_of(command.args, [&](const Arg& arg) { return arg.opt_id == id; });
    };
    bool full_bmi = has(clang_options::OPT__precompile) && !has(clang_options::OPT_fmodule_output);

    auto local = [](const Arg& arg) {
        return LocalArg{
            .opt_id = arg.opt_id,
            .cls = arg.cls,
            .alias = arg.alias,
            .spelling = arg.spelling,
            .values = {arg.values.begin(), arg.values.end()}
        };
    };
    std::vector<LocalArg> staged;
    auto stage = [&](llvm::ArrayRef<Arg> args) {
        for(auto& arg: args) {
            if(!(full_bmi && arg.opt_id == clang_options::OPT_fmodules_reduced_bmi)) {
                staged.push_back(local(arg));
            }
        }
    };
    stage(query.args.take_front(query.forwarded));
    /// The command's own user content never fed the query, nor did what the
    /// frontend takes as written. They go where the driver puts them, before
    /// what -Xclang passes on: CMake's `-Xclang -include -Xclang
    /// cmake_pch.hxx` follows the -includes of the command's flags.
    for(auto& arg: command.args) {
        if(arg.cls != ArgClass::UserContent && !frontend_takes_as_written(arg, command.family)) {
            continue;
        }
        auto& staged_arg = staged.emplace_back(local(arg));
        for(auto& value: staged_arg.values) {
            if(auto ours = ours_for(arg.opt_id, value, query.job_resource_dir)) {
                value = commands.intern(*ours).data();
            }
        }
    }
    stage(query.args.drop_front(query.forwarded));

    resolved_configs.push_back({.frontend = &query, .args = commands.persist(staged)});
    return static_cast<ResolvedID>(resolved_configs.size() - 1);
}

struct Toolchain::QueryAdmission {
    enum class Kind : std::uint8_t { Hit, CoolingDown, Ready };

    Kind kind;
    std::string key;
    /// Ready: whether the query runs in the command's directory.
    bool cwd_sensitive = false;
    /// CoolingDown: the cached failure.
    std::string error;
};

Toolchain::QueryAdmission Toolchain::admit_query(CommandID id, InputKind input) {
    auto pk = query_key(id, input);
    QueryAdmission admission{.kind = QueryAdmission::Kind::Hit, .key = std::move(pk.key)};
    if(queries.contains(admission.key)) {
        return admission;
    }
    // Past its cooldown a failure may have been transient: the real query
    // runs again.
    if(auto failed_it = failed.find(admission.key);
       failed_it != failed.end() &&
       failed_it->second.record.stops(query_policy, FailureRecord::Clock::now())) {
        admission.kind = QueryAdmission::Kind::CoolingDown;
        admission.error = failed_it->second.record.cause;
        return admission;
    }
    admission.kind = QueryAdmission::Kind::Ready;
    admission.cwd_sensitive = pk.cwd_sensitive;
    return admission;
}

QuerySpec Toolchain::query_spec(const QueryAdmission& admission, CommandID id, InputKind input) {
    auto& command = commands.get(id);
    auto argv = query_argv(command);
    return {
        .argv = std::move(argv.argv),
        .command = &command,
        .slot = argv.slot,
        .kind = input.value,
        .cwd = query_cwd(admission.cwd_sensitive ? command.directory
                                                 : llvm::StringRef(commands.project_root())),
    };
}

void Toolchain::commit_query(llvm::StringRef key,
                             CommandID id,
                             InputKind input,
                             const std::expected<Job, std::string>& job,
                             bool missing) {
    if(!job) {
        auto& entry = failed[key];
        entry.record.fail(query_policy, {}, job.error(), FailureRecord::Clock::now());
        entry.command = id;
        entry.input = input;
        entry.missing = missing;
        return;
    }
    if(failed.erase(key)) {
        answered_late += 1;
    }
    queries.try_emplace(key, frontend(*job, commands.get(id)));
}

bool Toolchain::query_failed(CommandID id, InputKind input) {
    return !failed.empty() && commands.get(id).query_toolchain &&
           failed.contains(query_key(id, input).key);
}

bool Toolchain::retry_failed() {
    std::vector<CommandRef> due;
    auto now = FailureRecord::Clock::now();
    for(auto& entry: failed) {
        if(entry.second.missing && !entry.second.record.stops(query_policy, now)) {
            due.push_back({.command = entry.second.command,
                           .language = entry.second.input,
                           .source = CommandSource::Database});
        }
    }
    warm(due, {.retry = true});
    bool answered = answered_late != reported_late;
    reported_late = answered_late;
    return answered;
}

std::expected<Toolchain::ResolvedCommand, std::string> Toolchain::resolve(CommandID id,
                                                                          InputKind input) {
    auto synth_key = std::pair{static_cast<std::uint32_t>(id), input.value};
    if(auto it = synth_cache.find(synth_key); it != synth_cache.end()) {
        return ResolvedCommand{.id = it->second};
    }

    auto admission = admit_query(id, input);
    if(admission.kind == QueryAdmission::Kind::CoolingDown) {
        return read_unqueried(id, input, std::move(admission.error));
    }
    if(admission.kind == QueryAdmission::Kind::Ready) {
        bool runs_compiler = commands.get(id).query_toolchain;
        if(runs_compiler) {
            LOG_DEBUG("Asking {} for its settings for {}, not asked at the start",
                      commands.get(id).driver,
                      input.value);
        }

        auto spec = query_spec(admission, id, input);
        QueryBatch batch;
        auto [ended] = async::run(query_one(spec, batch));
        auto& result = *ended;
        commit_query(admission.key, id, input, result, !result && !driver_found(spec));
        if(!result) {
            if(runs_compiler) {
                LOG_WARN(
                    "Cannot ask the compiler for its default settings: {}; clang's own "
                    "driver reads its command",
                    result.error());
            }
            return read_unqueried(id, input, std::move(result.error()));
        }
    }

    auto resolved_id = synthesize(id, queries.find(admission.key)->second);
    synth_cache.try_emplace(synth_key, resolved_id);
    return ResolvedCommand{.id = resolved_id};
}

std::expected<Toolchain::ResolvedCommand, std::string>
    Toolchain::read_unqueried(CommandID id, InputKind input, std::string failure) {
    // Not cached under the command itself: a failure is retried once its
    // cooldown passes.
    if(!commands.get(id).query_toolchain) {
        return std::unexpected(std::move(failure));
    }
    auto resolved = resolve(apply_edits(commands, files, id, {.query_toolchain = false}), input);
    if(!resolved) {
        return std::unexpected(
            std::format("{}; clang's own driver cannot read the command either: {}",
                        failure,
                        resolved.error()));
    }
    return ResolvedCommand{.id = resolved->id, .query_failure = std::move(failure)};
}

void Toolchain::warm(llvm::ArrayRef<CommandRef> refs, WarmOptions options) {
    struct Pending {
        std::string key;
        CommandID command;
        InputKind input;
        QuerySpec spec;

        /// A build chose the compiler: the builtin command's names one
        /// nobody did, and its query failing is no error.
        bool chosen = false;
    };

    llvm::StringMap<std::size_t> seen;
    std::vector<Pending> pending;

    for(auto& ref: refs) {
        auto admission = admit_query(ref.command, ref.language);
        if(admission.kind != QueryAdmission::Kind::Ready) {
            continue;
        }
        bool chosen = ref.source != CommandSource::Fallback;
        auto [it, inserted] = seen.try_emplace(admission.key, pending.size());
        if(!inserted) {
            pending[it->second].chosen |= chosen;
            continue;
        }
        auto spec = query_spec(admission, ref.command, ref.language);
        pending.push_back(
            {std::move(admission.key), ref.command, ref.language, std::move(spec), chosen});
    }

    if(pending.empty()) {
        return;
    }

    auto total = pending.size();
    LOG_DEBUG("Querying {} for default settings", counted(total, "compiler", "compilers"));

    // Run the queries concurrently on a local event loop. The interface
    // stays synchronous: block until all complete, then fill the cache here.
    struct QueryResult {
        std::string key;
        CommandID command;
        InputKind input;
        std::expected<Job, std::string> result;
        bool chosen;
        bool missing;
    };

    // The query is moved into the coroutine frame as a parameter, so the
    // argument references stay valid for the coroutine's whole lifetime.
    auto make_task =
        [](Pending p, async::semaphore& drivers, QueryBatch& batch) -> async::task<QueryResult> {
        auto running = co_await drivers.scoped_acquire();
        auto result = co_await query_one(p.spec, batch);
        bool missing = !result && !driver_found(p.spec);
        co_return QueryResult{std::move(p.key),
                              p.command,
                              p.input,
                              std::move(result),
                              p.chosen,
                              missing};
    };

    auto query_all = [&]() -> async::task<kotatsu::small_vector<QueryResult>> {
        // A database whose commands key thousands of queries would otherwise
        // run thousands of drivers at once.
        async::semaphore drivers(sys::parallelism());
        QueryBatch batch;
        std::vector<async::task<QueryResult>> tasks;
        tasks.reserve(pending.size());
        for(auto& p: pending) {
            tasks.push_back(make_task(std::move(p), drivers, batch));
        }

        co_return co_await async::when_all(std::move(tasks));
    };
    auto [results] = async::run(query_all());

    std::size_t succeeded = 0;
    for(auto& o: *results) {
        if(o.result) {
            succeeded += 1;
        } else if(options.retry) {
            LOG_DEBUG("The compiler still cannot be asked: {}", o.result.error());
        } else if(o.chosen) {
            LOG_WARN(
                "Cannot ask the compiler for its default settings: {}; clang's own "
                "driver reads its commands",
                o.result.error());
        } else {
            LOG_WARN("Cannot ask clice's default compiler for its settings: {}", o.result.error());
        }
        commit_query(o.key, o.command, o.input, o.result, o.missing);
    }

    if(options.retry) {
        if(succeeded != 0) {
            LOG_INFO("{} that could not be asked before answered",
                     counted(succeeded, "compiler", "compilers"));
        }
        return;
    }
    LOG_INFO("Queried {} for default settings; {} failed",
             counted(total, "compiler", "compilers"),
             total - succeeded);
}

Toolchain::RenderedCommand Toolchain::render(const CommandRef& ref,
                                             llvm::StringRef path,
                                             const RenderOptions& opts) {
    auto resolution = resolve(ref.command, ref.language);
    if(!resolution) {
        LOG_WARN("Cannot read the command of {} as its compiler does: {}",
                 files.resolve(ref.file),
                 resolution.error());
        return {.argv = render_for_clang(ref, path, opts)};
    }

    auto& rc = resolved(resolution->id);
    auto source = commands.intern(path);

    std::vector<const char*> argv;
    argv.reserve(rc.args.size() + 8);
    argv.push_back(rc.frontend->driver);
    argv.push_back("-cc1");
    argv.push_back("-main-file-name");
    // path::filename returns a suffix of the interned path (a pointer
    // into the same buffer), so .data() is null-terminated.
    argv.push_back(path::filename(source).data());

    auto emit = [&](llvm::StringRef fragment) {
        argv.push_back(commands.intern(fragment).data());
    };

    std::size_t last_user_content = argv.size();
    auto left_out = opts.left_out(rc.args);
    for(auto [i, arg]: views::enumerate(rc.args)) {
        if(left_out[i] ||
           (!opts.shadowed_dirs && names_search_dir(arg.opt_id) &&
            ranges::contains(rc.frontend->shadowed, llvm::StringRef(arg.values[0])))) {
            continue;
        }
        render_arg(arg, emit);
        if(arg.cls == ArgClass::UserContent) {
            last_user_content = argv.size();
        }
    }

    if(opts.preamble) {
        argv.insert(argv.begin() + last_user_content, {"-include", opts.preamble});
    }

    argv.push_back(source.data());
    return {.argv = std::move(argv),
            .query_failure = std::move(resolution->query_failure),
            .hidden = rc.frontend->hidden ? *rc.frontend->hidden : vfs::HiddenFiles{}};
}

std::vector<const char*> Toolchain::render_for_clang(const CommandRef& ref,
                                                     llvm::StringRef path,
                                                     const RenderOptions& opts) {
    auto& command = commands.get(ref.command);
    auto driver = commands.render_driver(ref, path, opts);
    // Resolving the command read it already, and logged what the driver said.
    auto reading = read_as_clang({driver.begin(), driver.end()},
                                 option_family(command),
                                 command.directory,
                                 /*quiet=*/true);
    std::vector<const char*> argv;
    argv.reserve(reading.argv.size());
    for(auto& arg: reading.argv) {
        argv.push_back(commands.intern(arg).data());
    }
    return argv;
}

void Toolchain::learn_search_dirs(llvm::ArrayRef<CommandRef> refs) {
    struct Dir {
        llvm::StringRef value;
        bool known = false;
        std::optional<CanonicalPath> identity;
        bool builtin_headers = false;
    };
    llvm::StringSet<> seen;
    std::vector<Dir> dirs;
    for(auto& ref: refs) {
        for(auto& arg: commands.get(ref.command).args) {
            if(!names_search_dir(arg.opt_id) && arg.opt_id != clang_options::OPT_iquote &&
               arg.opt_id != clang_options::OPT__SLASH_imsvc) {
                continue;
            }
            for(llvm::StringRef value: arg.values) {
                if(!path::is_absolute(value) || !seen.insert(value).second) {
                    continue;
                }
                bool known = files.knows(Spelling::absolute(value));
                if(!known || !builtin_header_dirs.contains(value)) {
                    dirs.push_back({.value = value, .known = known});
                }
            }
        }
    }
    auto learn = [&]() -> async::task<> {
        constexpr std::size_t batch = 256;
        async::task_group<> learning;
        for(std::size_t first = 0; first < dirs.size(); first += batch) {
            learning.spawn([](llvm::MutableArrayRef<Dir> dirs) -> async::task<> {
                co_await async::queue([&] {
                    for(auto& dir: dirs) {
                        if(!dir.known) {
                            dir.identity.emplace(Spelling::absolute(dir.value));
                        }
                        dir.builtin_headers = holds_builtin_headers(dir.value);
                    }
                });
            }(llvm::MutableArrayRef(dirs).slice(first, std::min(batch, dirs.size() - first))));
        }
        co_await learning.join();
    };
    async::run(learn());
    for(auto& dir: dirs) {
        if(dir.identity) {
            files.intern(Spelling::absolute(dir.value), *dir.identity);
        }
        builtin_header_dirs.try_emplace(dir.value, dir.builtin_headers);
    }
}

SearchConfig Toolchain::search_config(const CommandRef& ref) {
    auto& command = commands.get(ref.command);
    llvm::StringRef directory = command.directory;
    auto resolution = resolve(ref.command, ref.language);
    if(!resolution) {
        return extract_search_config(command.args, directory, files);
    }
    auto [it, inserted] = search_configs.try_emplace(resolution->id);
    if(inserted) {
        it->second = extract_search_config(resolved(resolution->id).args, directory, files);
        it->second.hidden = resolved(resolution->id).frontend->hidden;
        // A directory the command names is the user's, however the driver
        // passes it on: clang-cl's /imsvc reaches cc1 as -internal-isystem.
        llvm::StringSet<> named;
        auto base = Spelling::absolute(directory);
        for(auto& arg: command.args) {
            for(auto& value: arg.values) {
                named.insert(Spelling(value, base).str());
            }
        }
        for(auto& dir: it->second.dirs) {
            dir.driver = dir.driver && !named.contains(dir.path);
            if(dir.driver) {
                files.disk.add_package(files.resolve(files.intern(Spelling::absolute(dir.path))));
            }
        }
    }
    return it->second;
}

}  // namespace clice

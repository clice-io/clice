module;

#include "support/prelude.h"

#include "support/logging.macros.h"

module clice;

import :command.argument;
import :command.edit;
import :command.search_config;
import :command.table;
import :command.toolchain;
import :command.toolchain_query;
import :command.toolchains.clang;
import :command.toolchains.nvcc;
import :support.logging;
import :vfs.file_system;
import :vfs.path;

namespace clice {

namespace {

/// The query working directory, empty when the wanted one does not exist
/// (a stale CDB directory, or a foreign-platform path): querying from the
/// process cwd degrades the answer instead of failing the spawn.
std::string query_cwd(llvm::StringRef wanted) {
    if(wanted.empty() || !vfs::is_directory(wanted)) {
        return {};
    }
    return wanted.str();
}

/// Every component of a search-path-style environment value is absolute and
/// non-empty (an empty component means the cwd).
bool components_all_absolute(llvm::StringRef value) {
    llvm::SmallVector<llvm::StringRef, 16> parts;
    value.split(parts, path::list_separator, -1, /*KeepEmpty=*/true);
    return ranges::all_of(parts, [](llvm::StringRef part) {
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
            if(auto value = llvm::sys::Process::GetEnv(name)) {
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
            if(auto value = llvm::sys::Process::GetEnv(name)) {
                if(!components_all_absolute(*value))
                    return false;
            }
        }
        /// A single path, not a list.
        if(auto prefix = llvm::sys::Process::GetEnv("GCC_EXEC_PREFIX")) {
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
    return id == option::OPT_I || id == option::OPT_isystem || id == option::OPT_idirafter ||
           id == option::OPT_internal_isystem || id == option::OPT_internal_externc_isystem;
}

/// clice's own counterpart of a path another clang's job or the command
/// names, if it has one: the job's resource directory (`job_resource_dir`)
/// and its builtin headers, or another clang's builtin header directory a
/// search option names (the command's `-isystem <clang>/lib/clang/23/include`,
/// zig's lib/include). The rest of a resource tree stays external —
/// sanitizer ignorelists under share/, runtime libraries under lib/: the
/// driver verified those files exist there, clice's tree ships include/
/// alone, and cc1 aborts the process on an ignorelist it cannot open.
std::optional<std::string> ours_for(unsigned id,
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
    if(names_search_dir(id) && value.rtrim("/\\") != builtin_headers &&
       vfs::exists(path::join(value, "__stddef_max_align_t.h"))) {
        return builtin_headers;
    }
    return std::nullopt;
}

/// The files under `dir`, by their paths relative to it, `/`-separated.
std::vector<std::string> files_under(const Spelling& dir) {
    std::vector<std::string> names;
    vfs::walk(dir, [&](const vfs::Entry& entry) {
        if(entry.type == llvm::sys::fs::file_type::directory_file) {
            return true;
        }
        names.push_back(Spelling::absolute(entry.path).str().substr(dir.str().size() + 1));
        return false;
    });
    return names;
}

/// clice's builtin headers, by their paths relative to its resource
/// directory's include/.
const llvm::StringSet<>& resource_headers() {
    const static auto headers = [] {
        llvm::StringSet<> headers;
        for(auto& name: files_under(Spelling::absolute(path::join(resource_dir(), "include")))) {
            headers.insert(name);
        }
        return headers;
    }();
    return headers;
}

/// What the compiles of a job whose search list is `search` do not see of
/// the builtin headers of another compiler in its `shadowed` directories,
/// clang's own placed right before each: those clang has as well, and past
/// the directory, the copies of one whose own does not #include_next, which
/// that compiler never reaches (newlib's stdatomic.h behind GCC's).
std::shared_ptr<const vfs::HiddenFiles> hidden_files(const SearchConfig& search,
                                                     llvm::ArrayRef<Spelling> shadowed) {
    std::map<std::string, std::set<std::string>> hidden;
    for(auto& dir: shadowed) {
        auto found = ranges::find(search.dirs, dir.str(), &SearchDir::path);
        assert(found != search.dirs.end() && "a shadowed directory the job does not search");
        for(auto& name: files_under(dir)) {
            if(!resource_headers().contains(name)) {
                continue;
            }
            hidden[dir.str()].insert(name);
            std::optional<bool> defers;
            for(auto& later: llvm::make_range(std::next(found), search.dirs.end())) {
                if(!vfs::is_file(path::join(later.path, name))) {
                    continue;
                }
                if(!defers) {
                    auto text = vfs::read(path::join(dir, name));
                    defers = !text || (*text)->getBuffer().contains("include_next");
                }
                if(!*defers) {
                    hidden[later.path].insert(name);
                }
            }
        }
    }
    if(hidden.empty()) {
        return nullptr;
    }
    auto files = std::make_shared<vfs::HiddenFiles>();
    for(auto& [dir, names]: hidden) {
        files->dirs.push_back({
            .path = dir,
            .names = {names.begin(), names.end()}
        });
    }
    return files;
}

/// Which args feed the query (and its key): everything that may change
/// driver behavior. User-content never does, nor does the command's
/// action; unknown tokens feed it only as NVCC query tokens (junk from the
/// CDB must not fail an otherwise good query).
bool in_query_view(const Arg& arg, CompilerFamily family) {
    switch(arg.cls) {
        case ArgClass::Semantic: {
            // The query picks its own action; the command's (-fsyntax-only)
            // would go unused, an error under -Werror.
            auto opt = option::table().option(arg.opt_id);
            return !opt || !opt->matches(option::OPT_Action_Group);
        }
        case ArgClass::Diagnostics: return true;
        case ArgClass::Unknown:
            return family == CompilerFamily::NVCC && is_nvcc_query_flag(arg.spelling);
        case ArgClass::UserContent:
        case ArgClass::Codegen:
        case ArgClass::Discarded:
        case ArgClass::Input: return false;
    }
    std::unreachable();
}

}  // namespace

Toolchain::Toolchain(CommandTable& commands,
                     FileTable& files,
                     std::chrono::steady_clock::duration failed_retry) :
    commands(commands), files(files), failed_retry(failed_retry) {}

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
#ifdef _WIN32
    /// Windows resolves bare names against the current directory first
    /// (plus PATHEXT variants) — never exempt.
    bool driver_cwd_free = path::is_absolute(driver);
#else
    /// A bare name resolves through PATH alone (whose components the env
    /// condition below covers); a relative path is cwd-bound.
    bool driver_cwd_free =
        path::is_absolute(driver) || (!driver.contains('/') && !driver.contains('\\'));
#endif

    /// A known option's path values were anchored when the command was
    /// read (names_path); only an unknown one can still hold a
    /// relative path.
    bool values_clean = true;
    for(auto& arg: command.args) {
        if(arg.opt_id != option::OPT_UNKNOWN || !in_query_view(arg, command.family)) {
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
        out.key += std::to_string(keeps_alias(arg, command.family) ? arg.alias : arg.opt_id);
        out.key += '\0';
        /// All unknown options share one id; their identity is the spelling
        /// (the NVCC translation carries `-ccbin=<path>` through here, and
        /// two commands differing only in host compiler must not collide).
        if(arg.opt_id == option::OPT_UNKNOWN) {
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
    for(auto& arg: command.args) {
        if(arg.cls == ArgClass::Input) {
            out.slot = out.argv.size();
            continue;
        }
        if(!in_query_view(arg, command.family)) {
            continue;
        }
        if(arg.opt_id == option::OPT_UNKNOWN) {
            out.argv.push_back(arg.spelling);
            continue;
        }
        render_driver_arg(arg, command.family, emit);
    }

    return out;
}

Toolchain::Frontend Toolchain::frontend(const Job& job) {
    std::vector<std::string> tokens(job.args.begin() + 2, job.args.end());
    // An external clang may be newer than the linked one: a flag our cc1
    // does not know parses as unknown, and its values as inputs; both go.
    auto parse_options = kota::option::ParseOptions{.visibility = option::CC1Option};
    std::vector<kota::option::ParsedArg> kept;
    llvm::StringRef triple;
    llvm::StringRef job_resource_dir;
    for(auto& parsed: option::table().parse(tokens, parse_options)) {
        if(!parsed || parsed->id == option::OPT_UNKNOWN) {
            continue;
        }
        switch(parsed->id) {
            // The query's own action, input and outputs; the render gives
            // the real file its -main-file-name, and clice manages module
            // outputs itself (the driver derives -fmodule-output=<query>.pcm
            // for a module unit).
            case option::OPT_E:
            case option::OPT_fsyntax_only:
            case option::OPT_dM:
            case option::OPT_v:
            case option::OPT_o:
            case option::OPT_INPUT:
            case option::OPT_main_file_name:
            case option::OPT_fmodule_output:
            case option::OPT_fmodule_output_EQ: continue;
            case option::OPT_triple: triple = parsed->values[0]; break;
            // A trailing separator on the command's -resource-dir reaches
            // cc1 verbatim while the derived paths append without doubling
            // it.
            case option::OPT_resource_dir:
                job_resource_dir = llvm::StringRef(parsed->values[0]).rtrim("/\\");
                break;
        }
        // A newer external clang may give a known codegen option a value
        // the linked one cannot parse (-mframe-pointer=non-leaf-no-reserve);
        // the parse never needs them.
        if(is_codegen_option(parsed->id)) {
            continue;
        }
        kept.push_back(std::move(*parsed));
    }

    // The target's defaults stand in for the names it does not know.
    TargetNames names(triple);

    Frontend result{.driver = commands.intern(job.args[0]).data(),
                    .job_resource_dir = commands.intern(job_resource_dir)};
    result.own_resource_dir =
        llvm::Triple(triple).isWindowsGNUEnvironment() && vfs::is_directory(job_resource_dir);
    auto builtin_headers = path::join(resource_dir(), "include");
    std::vector<std::string> shadowed = job.shadowed;
    std::vector<LocalArg> staged;
    for(auto& arg: kept) {
        if(!arg.values.empty() && !names.knows(arg.id, arg.values[0])) {
            LOG_INFO("clang's target {} knows no CPU or ABI named {} ({}): its default stands in",
                     triple,
                     arg.values[0],
                     option::table().option(arg.id)->prefixed_name());
            continue;
        }
        auto local = commands.local_arg(arg, tokens);
        auto theirs = local;
        if(!result.own_resource_dir) {
            for(auto& value: local.values) {
                if(auto ours = ours_for(local.opt_id, value, job_resource_dir)) {
                    value = commands.intern(*ours).data();
                }
            }
        }
        staged.push_back(std::move(local));
        // Another clang's builtin headers stay right after ours, which
        // shadow them: what only theirs has is found there.
        if(names_search_dir(theirs.opt_id) &&
           llvm::StringRef(staged.back().values[0]) == builtin_headers &&
           llvm::StringRef(theirs.values[0]) != builtin_headers &&
           vfs::is_directory(theirs.values[0])) {
            shadowed.push_back(theirs.values[0]);
            staged.push_back(std::move(theirs));
        }
    }
    staged.push_back({.opt_id = option::OPT_fsyntax_only, .cls = ArgClass::Semantic});
    result.args = commands.persist(staged);
    if(!shadowed.empty()) {
        auto base = Spelling::cwd();
        std::vector<Spelling> spelled;
        for(auto& dir: shadowed) {
            spelled.emplace_back(dir, base);
        }
        result.hidden = hidden_files(extract_search_config(result.args, base), spelled);
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
    bool full_bmi = has(option::OPT__precompile) && !has(option::OPT_fmodule_output) &&
                    !has(option::OPT_fmodule_output_EQ);

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
    for(auto& arg: query.args) {
        if(!(full_bmi && arg.opt_id == option::OPT_fmodules_reduced_bmi)) {
            staged.push_back(local(arg));
        }
    }
    /// The command's own user content never fed the query.
    for(auto& arg: command.args) {
        if(arg.cls != ArgClass::UserContent) {
            continue;
        }
        auto& staged_arg = staged.emplace_back(local(arg));
        if(!query.own_resource_dir) {
            for(auto& value: staged_arg.values) {
                if(auto ours = ours_for(arg.opt_id, value, query.job_resource_dir)) {
                    value = commands.intern(*ours).data();
                }
            }
        }
    }

    resolved_configs.push_back(
        {.driver = query.driver, .args = commands.persist(staged), .hidden = query.hidden});
    return static_cast<ResolvedID>(resolved_configs.size() - 1);
}

struct Toolchain::QueryAdmission {
    enum class Kind : std::uint8_t { Hit, CoolingDown, Ready };

    Kind kind;
    std::string key;
    /// Ready: the query to run.
    QuerySpec spec;
    /// CoolingDown: the cached failure.
    std::string error;
};

Toolchain::QueryAdmission Toolchain::admit_query(CommandID id, InputKind input) {
    auto pk = query_key(id, input);
    QueryAdmission admission{.kind = QueryAdmission::Kind::Hit, .key = std::move(pk.key)};
    if(queries.contains(admission.key)) {
        return admission;
    }
    if(auto failed_it = failed.find(admission.key); failed_it != failed.end()) {
        if(std::chrono::steady_clock::now() - failed_it->second.second < failed_retry) {
            admission.kind = QueryAdmission::Kind::CoolingDown;
            admission.error = failed_it->second.first;
            return admission;
        }
        // Cooldown over: the failure may have been transient — retry the
        // real query (cf. BlameBudget's bounded-burn revival).
        failed.erase(failed_it);
    }

    auto& command = commands.get(id);
    auto argv = query_argv(command);
    admission.kind = QueryAdmission::Kind::Ready;
    admission.spec.argv = std::move(argv.argv);
    admission.spec.slot = argv.slot;
    admission.spec.kind = input.value;
    admission.spec.family = command.family;
    admission.spec.query_toolchain = command.query_toolchain;
    admission.spec.cwd = query_cwd(pk.cwd_sensitive ? command.directory
                                                    : llvm::StringRef(commands.workspace_root()));
    return admission;
}

void Toolchain::commit_query(llvm::StringRef key, const std::expected<Job, std::string>& job) {
    if(!job) {
        failed.try_emplace(key, std::pair{job.error(), std::chrono::steady_clock::now()});
        return;
    }
    queries.try_emplace(key, frontend(*job));
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
            LOG_WARN("Toolchain query miss: driver={} kind={}",
                     commands.get(id).driver,
                     input.value);
        }

        auto [ended] = kota::run(query_one(admission.spec));
        auto& result = *ended;
        commit_query(admission.key, result);
        if(!result) {
            if(runs_compiler) {
                LOG_WARN(
                    "The compiler {} could not be queried: {}; clang's own driver reads its "
                    "command",
                    commands.get(id).driver,
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

void Toolchain::warm(llvm::ArrayRef<CommandRef> refs) {
    struct Pending {
        std::string key;
        QuerySpec spec;
    };

    llvm::StringMap<bool> seen;
    std::vector<Pending> pending;

    for(auto& ref: refs) {
        auto admission = admit_query(ref.command, ref.input);
        if(admission.kind != QueryAdmission::Kind::Ready ||
           !seen.try_emplace(admission.key, true).second) {
            continue;
        }
        pending.push_back({std::move(admission.key), std::move(admission.spec)});
    }

    if(pending.empty()) {
        return;
    }

    auto total = pending.size();
    LOG_INFO("Warming toolchain cache: {} unique queries", total);

    // Run the queries concurrently on a local event loop. The interface
    // stays synchronous: block until all complete, then fill the cache here.
    struct QueryOutcome {
        std::string key;
        std::expected<Job, std::string> result;
    };

    // The query is moved into the coroutine frame as a parameter, so the
    // argument references stay valid for the coroutine's whole lifetime.
    auto make_task = [](Pending p, kota::semaphore& drivers) -> kota::task<QueryOutcome> {
        auto running = co_await drivers.scoped_acquire();
        auto result = co_await query_one(p.spec);
        co_return QueryOutcome{std::move(p.key), std::move(result)};
    };

    auto query_all = [&]() -> kota::task<kota::small_vector<QueryOutcome>> {
        // A database whose commands key thousands of queries would otherwise
        // run thousands of drivers at once.
        kota::semaphore drivers(kota::sys::parallelism());
        std::vector<kota::task<QueryOutcome>> tasks;
        tasks.reserve(pending.size());
        for(auto& p: pending) {
            tasks.push_back(make_task(std::move(p), drivers));
        }

        co_return co_await kota::when_all(std::move(tasks));
    };
    auto [outcomes] = kota::run(query_all());

    std::size_t succeeded = 0;
    for(auto& o: *outcomes) {
        if(o.result) {
            succeeded += 1;
        } else {
            LOG_ERROR("Toolchain query failed: {}", o.result.error());
        }
        commit_query(o.key, o.result);
    }

    LOG_INFO("Toolchain cache warmed: {} succeeded, {} failed", succeeded, total - succeeded);
}

Toolchain::RenderedCommand Toolchain::render(const CommandRef& ref,
                                             llvm::StringRef path,
                                             const RenderOptions& opts) {
    auto resolution = resolve(ref.command, ref.input);
    if(!resolution) {
        LOG_WARN("Toolchain resolve failed for {}: {}",
                 files.resolve(ref.file),
                 resolution.error());
        return {.argv = render_for_clang(ref, path, opts)};
    }

    auto& rc = resolved(resolution->id);
    auto source = commands.intern(path);

    std::vector<const char*> argv;
    argv.reserve(rc.args.size() + 8);
    argv.push_back(rc.driver);
    argv.push_back("-cc1");
    argv.push_back("-main-file-name");
    // path::filename returns a suffix of the interned path (a pointer
    // into the same buffer), so .data() is null-terminated.
    argv.push_back(path::filename(source).data());

    auto emit = [&](llvm::StringRef fragment) {
        argv.push_back(commands.intern(fragment).data());
    };

    std::size_t last_user_content = argv.size();
    std::size_t includes = 0;
    for(auto& arg: rc.args) {
        if(arg.opt_id == option::OPT_include) {
            includes += 1;
            if(includes > opts.kept_includes) {
                continue;
            }
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
            .hidden = rc.hidden ? *rc.hidden : vfs::HiddenFiles{}};
}

std::vector<const char*> Toolchain::render_for_clang(const CommandRef& ref,
                                                     llvm::StringRef path,
                                                     const RenderOptions& opts) {
    auto& command = commands.get(ref.command);
    auto driver = commands.render_driver(ref, path, opts);
    // Resolving the command read it already, and logged what the driver said.
    auto reading = read_as_clang({driver.begin(), driver.end()},
                                 command.family,
                                 command.directory,
                                 /*quiet=*/true);
    std::vector<const char*> argv;
    argv.reserve(reading.argv.size());
    for(auto& arg: reading.argv) {
        argv.push_back(commands.intern(arg).data());
    }
    return argv;
}

SearchConfig Toolchain::search_config(const CommandRef& ref) {
    auto& command = commands.get(ref.command);
    llvm::StringRef directory = command.directory;
    auto resolution = resolve(ref.command, ref.input);
    if(!resolution) {
        return extract_search_config(command.args, directory);
    }
    auto [it, inserted] = search_configs.try_emplace(resolution->id);
    if(inserted) {
        it->second = extract_search_config(resolved(resolution->id).args, directory);
        it->second.hidden = resolved(resolution->id).hidden;
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
                files.disk.add_package(CanonicalPath(Spelling::absolute(dir.path)).str());
            }
        }
    }
    return it->second;
}

}  // namespace clice

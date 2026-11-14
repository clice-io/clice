module clice;

import :command.argument;
import :command.normalize;
import :command.table;
import :command.toolchains.cl;
import :command.toolchains.clang;
import :command.toolchains.gcc;
import :command.toolchains.nvcc;
import :command.toolchains.own_options;
import :support.process;
import :vfs.path;

namespace clice {

namespace {

constexpr llvm::StringLiteral ccbin_prefix = "-ccbin=";
constexpr llvm::StringLiteral target_directory_prefix = "--target-directory=";
constexpr llvm::StringLiteral allow_unsupported_flag = "--allow-unsupported-compiler";
constexpr llvm::StringLiteral gpu_arch_prefix = "-arch=";

namespace nvcc_options {

/// nvcc's options (nvcc_options.inc), with ids after the table's input and
/// unknown option, as clang's are (clang_options::ID).
enum ID : std::uint32_t {
    OPT_INPUT = OptionSpellings::input,
    OPT_UNKNOWN = OptionSpellings::unknown,
#define NVCC_OPTION(ID, NAME, SHORT_NAME, VALUE) OPT_##ID,
#include "command/toolchains/nvcc_options.inc"
#undef NVCC_OPTION
    OPT_END,
};

}  // namespace nvcc_options

/// How an nvcc option takes its value (nvcc_options.inc).
enum class Value : std::uint8_t {
    None,
    Separate,
    Joined,
};

const cli::OptTable& nvcc_table() {
    struct Option {
        std::string_view name;
        std::string_view short_name;
        Value value;
    };
    using enum Value;
    constexpr Option options[] = {
#define NVCC_OPTION(ID, NAME, SHORT_NAME, VALUE) {NAME, SHORT_NAME, VALUE},
#include "command/toolchains/nvcc_options.inc"
#undef NVCC_OPTION
    };
    const static OptionSpellings table(nvcc_options::OPT_END, [&] {
        std::vector<OptionSpellings::Entry> spellings;
        for(auto [index, option]: views::enumerate(options)) {
            auto id = static_cast<std::uint32_t>(OptionSpellings::first_option + index);
            for(auto [prefix, name]: {
                    std::pair{"--", option.name      },
                    std::pair{"-",  option.short_name}
            }) {
                if(name.empty()) {
                    continue;
                }
                auto spelled = std::string(prefix) + std::string(name);
                if(option.value == None) {
                    spellings.push_back({spelled, cli::OptionKind::Flag, id});
                    continue;
                }
                bool joined = option.value == Joined && llvm::StringRef(prefix) == "-";
                spellings.push_back(
                    {spelled,
                     joined ? cli::OptionKind::JoinedOrSeparate : cli::OptionKind::Separate,
                     id});
                spellings.push_back({spelled + "=", cli::OptionKind::Joined, id});
            }
        }
        return spellings;
    }());
    return table.table();
}

/// One GPU architecture named inside an -arch/-gencode value.
struct ArchToken {
    unsigned number = 0;

    /// 'a' (arch-specific) or 'f' (family) variant, 0 for the plain form.
    char suffix = 0;

    bool operator==(const ArchToken&) const = default;
};

/// Scan a spec like "compute_90a" or "arch=compute_90a" for sm_NN /
/// compute_NN tokens. `lto_NN` entries name LTO intermediates, not
/// architectures, and are ignored.
void collect_archs(llvm::StringRef spec, llvm::SmallVectorImpl<ArchToken>& out) {
    for(llvm::StringRef prefix: {"sm_", "compute_"}) {
        std::size_t pos = 0;
        while((pos = spec.find(prefix, pos)) != llvm::StringRef::npos) {
            pos += prefix.size();
            auto rest = spec.substr(pos);

            unsigned number = 0;
            std::size_t len = 0;
            while(len < rest.size() && llvm::isDigit(rest[len])) {
                number = number * 10 + (rest[len] - '0');
                len += 1;
            }
            if(len == 0)
                continue;

            char suffix = 0;
            if(len < rest.size() && llvm::isAlpha(rest[len]))
                suffix = rest[len];

            out.push_back({number, suffix});
        }
    }
}

/// The newest architecture wins; at equal number the fuller feature set does
/// ('a' unlocks the arch-specific instructions the family 'f' and plain
/// forms hide, e.g. sm_90a for Hopper GMMA/TMA).
std::pair<unsigned, int> arch_rank(const ArchToken& arch) {
    int suffix_rank = arch.suffix == 'a' ? 2 : arch.suffix == 'f' ? 1 : 0;
    return {arch.number, suffix_rank};
}

std::string select_gpu_arch(llvm::ArrayRef<ArchToken> archs) {
    auto best = *ranges::max_element(archs, {}, arch_rank);

    auto result = std::format("sm_{}", best.number);
    if(best.suffix != 0)
        result += best.suffix;
    return result;
}

/// Split on unescaped commas, `\,` reading as a literal comma. Other
/// backslashes stay verbatim: they are path separators on Windows, and
/// nvcc's fuller Linux-side escape processing (a backslash escapes any next
/// character) would destroy every native path.
void split_list(llvm::StringRef value, llvm::SmallVectorImpl<std::string>& out) {
    std::string piece;
    for(std::size_t i = 0; i < value.size(); i += 1) {
        char c = value[i];
        if(c == '\\' && i + 1 < value.size() && value[i + 1] == ',') {
            piece += ',';
            i += 1;
            continue;
        }
        if(c == ',') {
            if(!piece.empty())
                out.push_back(std::move(piece));
            piece.clear();
            continue;
        }
        piece += c;
    }
    if(!piece.empty())
        out.push_back(std::move(piece));
}

/// The nvcc option the single argument `arg` is; none when it is no option
/// of nvcc's or misses its value.
std::optional<cli::ParsedArg> nvcc_option(llvm::StringRef arg) {
    std::array argv = {std::string_view(arg)};
    auto parsed = *nvcc_table().parse(argv).begin();
    if(!parsed || parsed->id < OptionSpellings::first_option) {
        return std::nullopt;
    }
    return std::move(*parsed);
}

/// Whether nvcc's option `id` is one only nvcc resolves (is_nvcc_query_flag).
bool is_query_option(std::uint32_t id) {
    using namespace nvcc_options;
    return id == OPT_compiler_bindir || id == OPT_allow_unsupported_compiler ||
           id == OPT_target_directory || id == OPT_gpu_architecture;
}

/// The PATH nvcc runs with when its host is cl, which it looks for on PATH
/// alone whatever `-ccbin` names: led by the directory `ccbin` names when a
/// path, else by the newest toolset Visual Studio installed when PATH has
/// no cl (an editor started clice outside a developer prompt). Empty to
/// keep PATH as it is.
std::string cl_search_path(llvm::StringRef ccbin) {
#ifdef _WIN32
    std::string directory;
    std::string found;
    if(path::is_absolute(ccbin)) {
        directory = fs::is_directory(ccbin) ? ccbin.str() : std::string(path::parent(ccbin));
    } else if(sys::find_program(found, "cl")) {
        directory = visual_studio_bin("cl");
    }
    if(auto path = env_string("PATH"); !directory.empty() && path) {
        directory += ';';
        directory += *path;
    }
    return directory;
#else
    return {};
#endif
}

/// The host compiler nvcc's dryrun names, as a path. The dryrun spells it
/// the way nvcc resolves it — often a bare name that only exists on nvcc's
/// own augmented PATH: that PATH, then next to the nvcc binary, then ours.
std::expected<std::string, std::string> find_nvcc_host(const NVCCDryrun& info,
                                                       llvm::StringRef nvcc) {
    llvm::StringRef host = info.host_compiler;
    if(path::is_absolute(host)) {
        return host.str();
    }
    auto directories = info.search_path;
    directories.emplace_back(path::parent(nvcc));
    std::string found;
    if(sys::find_program(found, host, {.directories = std::move(directories)}) &&
       sys::find_program(found, host)) {
        return std::unexpected(std::format("Cannot find nvcc host compiler: {}", host));
    }
    return found;
}

}  // namespace

bool is_nvcc_query_flag(llvm::StringRef arg) {
    auto option = nvcc_option(arg);
    return option && is_query_option(option->id);
}

std::optional<llvm::SmallVector<std::string>> options_files(llvm::ArrayRef<std::string> arguments,
                                                            std::size_t& i) {
    auto parsed = *nvcc_table().parse(arguments.drop_front(i)).begin();
    if(!parsed) {
        i = arguments.size();
        return std::nullopt;
    }
    i += parsed->next_index;
    if(parsed->id != nvcc_options::OPT_options_file) {
        return std::nullopt;
    }
    llvm::SmallVector<std::string> files;
    split_list(parsed->values[0], files);
    return files;
}

CompilerFamily nvcc_host(llvm::ArrayRef<std::string> args) {
    auto host = CompilerFamily::Unknown;
    for(auto& parsed: nvcc_table().parse(args)) {
        if(parsed && parsed->id == nvcc_options::OPT_compiler_bindir) {
            host = driver_family(parsed->values[0]);
        }
    }
    if(host != CompilerFamily::Unknown) {
        return host;
    }
#ifdef _WIN32
    return CompilerFamily::MSVC;
#else
    return CompilerFamily::GCC;
#endif
}

std::vector<std::string> translate_nvcc_command(llvm::ArrayRef<const char*> arguments,
                                                llvm::StringRef directory,
                                                CompilerFamily host,
                                                bool edit) {
    std::vector<std::string> result;
    result.emplace_back(arguments[0]);

    /// Stateful nvcc options are last-wins (`-rdc=true -rdc=false` compiles
    /// without relocatable device code), so they collect here and render
    /// once at the end. -gencode is the exception: entries accumulate.
    llvm::SmallVector<ArchToken> gencode_archs;
    llvm::SmallVector<ArchToken> arch_archs;
    llvm::StringRef arch_special;
    std::string host_compiler;
    std::string target_directory;
    llvm::StringRef default_stream;
    llvm::StringRef machine;
    llvm::StringRef optimize;
    bool allow_unsupported = false;
    std::optional<bool> rdc;
    bool ewp = false;
    bool relaxed_constexpr = false;
    bool extended_lambda = false;
    bool device_debug = false;
    bool use_fast_math = false;

    llvm::ArrayRef<const char*> args = arguments.drop_front();

    /// nvcc treats every preprocessor value as a comma-separated list,
    /// short spellings included: `-Ia,b` names two directories.
    auto emit_list = [&](llvm::StringRef flag, llvm::StringRef value) {
        llvm::SmallVector<std::string> pieces;
        split_list(value, pieces);
        for(auto& piece: pieces) {
            result.emplace_back(flag);
            result.emplace_back(std::move(piece));
        }
    };

    auto quoting = cl_mode(host) ? sys::cmd::Quoting::Windows : sys::cmd::Quoting::POSIX;
    using namespace nvcc_options;
    for(auto& parsed: nvcc_table().parse(args)) {
        // nvcc rejects an option missing its value, which only the last
        // argument can be.
        if(!parsed) {
            break;
        }
        llvm::StringRef value = parsed->values.empty() ? "" : parsed->values[0];
        switch(parsed->id) {
            // An input, or an option of the host compiler's nvcc passes on
            // (-forward-unknown-to-host-compiler); nvcc's own warnings clang
            // names alike.
            case OPT_INPUT:
            case OPT_UNKNOWN:
            case OPT_Wreorder:
            case OPT_Wno_deprecated_declarations:
            case OPT_Wconversion: result.emplace_back(args[parsed->index]); break;
            /// Only the `arch=` clause of a -gencode value names the virtual
            /// architecture the device front end compiles for; `code=`
            /// entries are ptxas targets (`-gencode arch=compute_80,code=sm_90`
            /// preprocesses with `__CUDA_ARCH__` == 800).
            case OPT_generate_code: {
                llvm::SmallVector<llvm::StringRef> pieces;
                value.split(pieces, ',', -1, false);
                for(llvm::StringRef piece: pieces) {
                    piece = piece.trim();
                    if(piece.consume_front("arch=")) {
                        collect_archs(piece, gencode_archs);
                    }
                }
                break;
            }
            case OPT_gpu_architecture:
                arch_archs.clear();
                arch_special = {};
                collect_archs(value, arch_archs);
                if(arch_archs.empty()) {
                    arch_special = value;
                }
                break;
            /// A bare program name resolves on PATH like nvcc would; only
            /// path-shaped values anchor to the compile directory, nvcc's
            /// working directory. A leading slash is absolute on POSIX and
            /// root-relative on Windows: never compile-directory-relative.
            case OPT_compiler_bindir: {
                bool path_shaped =
                    value.contains('/') || value.contains('\\') || value == "." || value == "..";
                host_compiler = path_shaped && !value.starts_with("/")
                                    ? path::join(directory, value)
                                    : value.str();
                break;
            }
            case OPT_allow_unsupported_compiler: allow_unsupported = true; break;
            case OPT_target_directory: target_directory = value; break;
            /// nvcc packs several host flags into one comma-separated value,
            /// under the same `\,` escape as every other list option
            /// (`-Xcompiler=-Wl\,-z\,defs` reaches the host as one flag).
            /// It writes each piece into the host's command line as it
            /// stands, which the host splits: a POSIX shell for GCC, the
            /// Windows rules after the program for cl (CMake's
            /// `-Xcompiler=" -Zi -Ob0 -Od /RTC1"`).
            case OPT_compiler_options: {
                llvm::SmallVector<std::string> pieces;
                split_list(value, pieces);
                for(auto& piece: pieces) {
                    auto line = split_command("host " + piece, quoting);
                    result.insert(result.end(),
                                  std::make_move_iterator(line.begin() + 1),
                                  std::make_move_iterator(line.end()));
                }
                break;
            }
            case OPT_std: result.emplace_back(("-std=" + value).str()); break;
            case OPT_stdlib: result.emplace_back(("-stdlib=" + value).str()); break;
            case OPT_x:
                result.emplace_back("-x");
                result.emplace_back(value == "cu" ? "cuda" : value.str());
                break;
            /// A value nvcc rejects pins nothing.
            case OPT_machine:
                if(value == "32" || value == "64") {
                    machine = value;
                }
                break;
            case OPT_m64: machine = "64"; break;
            case OPT_optimize: optimize = value; break;
            case OPT_disable_warnings: result.emplace_back("-w"); break;
            case OPT_relocatable_device_code: rdc = value == "true"; break;
            /// Separate compilation implies -rdc=true.
            case OPT_device_c: rdc = true; break;
            case OPT_extensible_whole_program: ewp = true; break;
            case OPT_default_stream: default_stream = value; break;
            /// Feature toggles whose only parse-visible effect is a macro.
            case OPT_expt_relaxed_constexpr: relaxed_constexpr = true; break;
            case OPT_extended_lambda:
            case OPT_expt_extended_lambda: extended_lambda = true; break;
            /// Consumed rather than passed through: clang reads a bare -G as
            /// its small-data-threshold option.
            case OPT_device_debug: device_debug = true; break;
            /// Device fast math has no preprocess-visible effect in nvcc
            /// (cicc swaps the transcendentals itself); clang's flag selects
            /// the same fast variants in its math wrapper via
            /// __CLANG_GPU_APPROX_TRANSCENDENTALS__.
            case OPT_use_fast_math: use_fast_math = true; break;
            /// Preprocessor list options, rewritten to the short spellings
            /// the CDB classification knows.
            case OPT_system_include: emit_list("-isystem", value); break;
            case OPT_pre_include: emit_list("-include", value); break;
            case OPT_include_path: emit_list("-I", value); break;
            case OPT_define_macro: emit_list("-D", value); break;
            case OPT_undefine_macro: emit_list("-U", value); break;
            case OPT_no_exceptions: result.emplace_back("-fno-exceptions"); break;
            case OPT_ftemplate_depth:
                result.emplace_back(("-ftemplate-depth=" + value).str());
                break;
            case OPT_ftemplate_backtrace_limit:
                result.emplace_back(("-ftemplate-backtrace-limit=" + value).str());
                break;
            /// What has no effect on the parse, dropped with its value:
            /// nvcc's outputs and actions, its device code generation,
            /// linking, the options of the tools it runs, and its options
            /// files, spliced in before (Normalizer::expand_response_files).
            default: break;
        }
    }

    /// nvcc injects its macros ahead of the user's preprocessor flags, so a
    /// later `-U` can undo them (`-dc -U__CUDACC_RDC__` parses without the
    /// RDC macro) — the synthetic ones render first.
    std::vector<std::string> prelude;
    if(rdc == true) {
        prelude.emplace_back("-fgpu-rdc");
        /// nvcc defines it; clang's -fgpu-rdc does not.
        prelude.emplace_back("-D__CUDACC_RDC__");
    } else if(edit && rdc == false) {
        prelude.emplace_back("-fno-gpu-rdc");
        prelude.emplace_back("-U__CUDACC_RDC__");
    }
    if(relaxed_constexpr)
        prelude.emplace_back("-D__CUDACC_RELAXED_CONSTEXPR__");
    if(extended_lambda)
        prelude.emplace_back("-D__CUDACC_EXTENDED_LAMBDA__");
    if(ewp)
        prelude.emplace_back("-D__CUDACC_EWP__");
    if(device_debug)
        prelude.emplace_back("-D__CUDACC_DEBUG__");
    if(use_fast_math)
        prelude.emplace_back("-fgpu-approx-transcendentals");
    /// The stream cancellation also renders ahead of the segment's own
    /// flags: the base's injected macro sits before the seam either way,
    /// and an explicit -D of the macro inside this edit must survive the
    /// -U, like it survives nvcc's absent injection.
    if(edit && !default_stream.empty() && default_stream != "per-thread")
        prelude.emplace_back("-UCUDA_API_PER_THREAD_DEFAULT_STREAM");
    result.insert(result.begin() + 1, prelude.begin(), prelude.end());

    /// The stream macro is nvcc's one exception: it lands after the user's
    /// preprocessor flags.
    if(default_stream == "per-thread")
        result.emplace_back("-DCUDA_API_PER_THREAD_DEFAULT_STREAM=1");

    /// nvcc's own -O/-m always land after the -Xcompiler payloads on the
    /// host line, beating them regardless of input order.
    if(!machine.empty())
        result.emplace_back(("-m" + machine).str());
    if(!optimize.empty())
        result.emplace_back(("-O" + optimize).str());

    /// nvcc compiles the union of the -gencode entries and the (last-wins)
    /// -arch choice, one device pass each — the newest of the union is the
    /// view. A non-numeric -arch next to -gencode entries joins nvcc's
    /// union too, but resolving it needs the dryrun; the numeric side
    /// approximates the pick.
    llvm::SmallVector<ArchToken> archs = gencode_archs;
    archs.append(arch_archs.begin(), arch_archs.end());
    if(!archs.empty()) {
        /// A pure -arch edit replaces the base's architectures: nvcc itself
        /// would union it with the base's -gencode entries, but an appended
        /// -arch reads as the user picking the view outright. -gencode
        /// edits accumulate like nvcc's own, emitted bare for
        /// collapse_gpu_arch_flags to resolve against the base's.
        if(edit && gencode_archs.empty())
            result.emplace_back("--no-offload-arch=all");
        result.emplace_back("--cuda-gpu-arch=" + select_gpu_arch(archs));
    } else if(!arch_special.empty()) {
        /// A non-numeric selection (native, all, all-major) only nvcc can
        /// resolve: it persists as a query token, the dryrun runs with it,
        /// and its cicc line then names the concrete architecture.
        if(edit)
            result.emplace_back("--no-offload-arch=all");
        result.emplace_back((llvm::Twine(gpu_arch_prefix) + arch_special).str());
    }

    if(!host_compiler.empty())
        result.emplace_back((llvm::Twine(ccbin_prefix) + host_compiler).str());
    if(allow_unsupported)
        result.emplace_back(allow_unsupported_flag.str());
    if(!target_directory.empty())
        result.emplace_back((llvm::Twine(target_directory_prefix) + target_directory).str());

    return result;
}

std::optional<llvm::SmallVector<std::size_t>>
    collapse_gpu_archs(llvm::ArrayRef<std::pair<ArchFlagKind, llvm::StringRef>> sequence) {
    struct ActiveArch {
        std::size_t index;
        llvm::SmallVector<ArchToken, 1> tokens;
        std::pair<unsigned, int> rank;
    };

    /// The arch flags still alive under clang's accumulate semantics:
    /// --no-offload-arch=all erases everything before it, a specific
    /// --no-offload-arch=sm_NN only its matches.
    llvm::SmallVector<ActiveArch> active;
    for(std::size_t i = 0; i < sequence.size(); i += 1) {
        auto [kind, value] = sequence[i];
        if(kind == ArchFlagKind::NoOffloadArch) {
            if(value == "all") {
                active.clear();
                continue;
            }
            llvm::SmallVector<ArchToken> removed;
            collect_archs(value, removed);
            if(removed.empty())
                return std::nullopt;
            auto matched = [&](const ArchToken& token) {
                return ranges::contains(removed, token);
            };
            for(std::size_t j = active.size(); j > 0;) {
                j -= 1;
                auto& entry = active[j];
                if(ranges::none_of(entry.tokens, matched))
                    continue;
                /// A flag naming both erased and surviving architectures
                /// cannot drop at flag granularity — bail like below.
                if(!ranges::all_of(entry.tokens, matched))
                    return std::nullopt;
                active.erase(active.begin() + j);
            }
            continue;
        }

        llvm::SmallVector<ArchToken> tokens;
        collect_archs(value, tokens);
        /// A value without an sm_NN/compute_NN token (a raw clang spelling
        /// like --offload-arch=native in a config append) is outside the
        /// ranking — leave the whole command to clang's own semantics.
        if(tokens.empty())
            return std::nullopt;
        auto rank = arch_rank(*ranges::max_element(tokens, {}, arch_rank));
        active.push_back({.index = i, .tokens = std::move(tokens), .rank = rank});
    }
    if(active.size() < 2)
        return llvm::SmallVector<std::size_t>{};

    auto best = ranges::max(active, {}, &ActiveArch::rank).rank;
    llvm::SmallVector<std::size_t> dropped;
    for(auto& arch: active) {
        if(arch.rank < best)
            dropped.push_back(arch.index);
    }
    return dropped;
}

std::vector<std::string> translate_nvcc_flags(Normalizer& normalizer,
                                              const Command& base,
                                              llvm::ArrayRef<std::string> flags,
                                              const Spelling& anchor,
                                              bool edit) {
    if(flags.empty()) {
        return {};
    }
    auto expanded = normalizer.expand_response_files(flags, anchor, CompilerFamily::NVCC);
    llvm::SmallVector<const char*, 32> argv = {base.driver};
    for(auto& flag: expanded) {
        argv.push_back(flag.c_str());
    }
    auto translated = translate_nvcc_command(argv, anchor, base.host, edit);
    return {std::make_move_iterator(translated.begin() + 1),
            std::make_move_iterator(translated.end())};
}

std::vector<std::string> translate_nvcc_removes(Normalizer& normalizer,
                                                const Command& base,
                                                llvm::ArrayRef<std::string> flags,
                                                const Spelling& anchor) {
    /// Remove patterns are an independent list, not one command: translated
    /// whole, nvcc's last-wins would swallow every alternative value of a
    /// stateful option but the last. Each pattern translates alone —
    /// standalone, so it reproduces exactly the flags the base translation
    /// emitted.
    std::vector<std::string> removes;
    for(auto& parsed: nvcc_table().parse(flags)) {
        // An option written without its value, last in the list, stays as
        // written: the edit takes it for every value.
        if(!parsed) {
            removes.push_back(flags[parsed.error().begin]);
            break;
        }
        /// A wildcard arch removal (`-arch=*`, `--generate-code=*`) must
        /// clear whichever form the translated base carries: numeric archs
        /// become `--cuda-gpu-arch=`, non-numeric selections persist as
        /// `-arch=` query tokens — rewrite to both wildcards.
        auto id = parsed->id;
        if((id == nvcc_options::OPT_gpu_architecture || id == nvcc_options::OPT_generate_code) &&
           parsed->values[0] == "*") {
            removes.insert(removes.end(), {"--cuda-gpu-arch=*", "-arch=*"});
            continue;
        }
        auto pattern =
            translate_nvcc_flags(normalizer,
                                 base,
                                 flags.slice(parsed->index, parsed->next_index - parsed->index),
                                 anchor,
                                 /*edit=*/false);
        removes.insert(removes.end(),
                       std::make_move_iterator(pattern.begin()),
                       std::make_move_iterator(pattern.end()));
    }
    return removes;
}

void collapse_gpu_arch_args(std::vector<LocalArg>& args) {
    llvm::SmallVector<std::pair<ArchFlagKind, llvm::StringRef>> sequence;
    llvm::SmallVector<std::size_t> positions;
    for(std::size_t i = 0; i < args.size(); i += 1) {
        auto& arg = args[i];
        std::optional<ArchFlagKind> kind;
        switch(arg.opt_id) {
            case clang_options::OPT_offload_arch_EQ: kind = ArchFlagKind::OffloadArch; break;
            case clang_options::OPT_no_offload_arch_EQ: kind = ArchFlagKind::NoOffloadArch; break;
        }
        if(kind && arg.values.size() == 1) {
            sequence.push_back({*kind, arg.values[0]});
            positions.push_back(i);
        }
    }

    auto dropped = collapse_gpu_archs(sequence);
    if(!dropped) {
        return;
    }
    for(auto index: *dropped | views::reverse) {
        args.erase(args.begin() + positions[index]);
    }
}

std::expected<NVCCDryrun, std::string> parse_nvcc_dryrun(llvm::StringRef output) {
    NVCCDryrun info;
    llvm::StringRef nvvm_dir;
    llvm::SmallVector<ArchToken> cicc_archs;
    std::vector<std::string> first_command;

    auto harvest_include_dirs = [&](llvm::ArrayRef<std::string> tokens) {
        for(std::size_t i = 0; i < tokens.size(); i += 1) {
            llvm::StringRef token = tokens[i];
            if(!token.consume_front("-I") && !token.consume_front("-isystem")) {
                continue;
            }
            if(token.empty() && i + 1 < tokens.size()) {
                i += 1;
                token = tokens[i];
            }
            info.include_dirs.emplace_back(token);
        }
    };

    // LIMITATION: nvcc's version macros (`__CUDACC_VER_*`) pass on: under
    // nvcc 12.8 or later, code taking their `>= 12.8` branch (CUTLASS) names
    // `__NV_ATOMIC_RELAXED`, defined by a CUDA header that clang's CUDA
    // wrapper headers replace, and reports it undeclared.
    auto harvest_defines = [](llvm::ArrayRef<std::string> tokens, std::vector<std::string>& out) {
        for(llvm::StringRef token: tokens) {
            if(!token.consume_front("-D"))
                continue;
            auto name = token.substr(0, token.find('='));
            if(name == "__CUDACC__" || name == "__CUDA_ARCH__" || name == "__CUDA_ARCH_LIST__")
                continue;
            out.emplace_back(token);
        }
    };

    llvm::SmallVector<llvm::StringRef> lines;
    output.split(lines, '\n', -1, false);

    for(llvm::StringRef line: lines) {
        line = line.trim();
        if(!line.consume_front("#$ "))
            continue;

        if(line.consume_front("TOP=")) {
            info.cuda_path = line.trim();
            continue;
        }

        if(line.consume_front("NVVMIR_LIBRARY_DIR=")) {
            nvvm_dir = line.trim();
            continue;
        }

        if(line.consume_front("PATH=")) {
            for(auto dir: sys::split_paths(line)) {
                if(!dir.empty()) {
                    info.search_path.emplace_back(dir);
                }
            }
            continue;
        }

        /// Remaining lines are either environment assignments (INCLUDES=...,
        /// CICC_PATH=..., no space before '=') or pipeline commands.
        auto head = line.take_until([](char c) { return c == ' '; });
        if(head.contains('='))
            continue;

        // Quoted for the system's shell: Windows paths keep their
        // backslashes.
        auto tokens = split_command(line);
        if(tokens.empty())
            continue;

        llvm::StringRef argv0 = tokens[0];
        auto program = path::filename(argv0);

        /// cudafe++ names the C++ dialect nvcc compiles in, cl's after an
        /// `ms_` (`--c++17`, `--ms_c++14`), and with a cl host the version
        /// of that cl (`--microsoft_version=1951`, its `_MSC_VER`).
        if(program == "cudafe++") {
            for(llvm::StringRef token: tokens) {
                if(token.consume_front("--microsoft_version=") && token.size() > 2) {
                    info.ms_version = (token.drop_back(2) + "." + token.take_back(2)).str();
                } else if((token.consume_front("--ms_") || token.consume_front("--")) &&
                          token.starts_with("c++")) {
                    info.cpp_dialect = token;
                }
            }
            continue;
        }

        /// A query carrying `-arch=all` runs one cicc per architecture:
        /// collect them all and pick by the same newest-wins policy as the
        /// translation.
        if(program == "cicc") {
            for(std::size_t i = 0; i + 1 < tokens.size(); i += 1) {
                if(llvm::StringRef(tokens[i]) == "-arch")
                    collect_archs(tokens[i + 1], cicc_archs);
            }
            continue;
        }

        auto tail = llvm::ArrayRef(tokens).drop_front();

        if(first_command.empty())
            first_command = tokens;

        bool is_preprocess = ranges::contains(tail, std::string_view("-E"));
        if(!is_preprocess)
            continue;

        bool is_device = ranges::any_of(tail, [](llvm::StringRef token) {
            return token.starts_with("-D__CUDA_ARCH__");
        });

        if(info.host_compiler.empty())
            info.host_compiler = argv0;

        harvest_defines(tail, is_device ? info.device_defines : info.host_defines);
        if(!is_device) {
            harvest_include_dirs(tail);
        }
    }

    /// A dryrun without `TOP=` still names the toolkit root indirectly:
    /// `NVVMIR_LIBRARY_DIR=` is `<root>/nvvm/libdevice`.
    auto nvvm = path::parent(nvvm_dir);
    if(info.cuda_path.empty() && path::filename(nvvm_dir) == "libdevice" &&
       path::filename(nvvm) == "nvvm") {
        info.cuda_path = path::parent(nvvm);
    }

    if(!cicc_archs.empty())
        info.default_arch = select_gpu_arch(cicc_archs);

    /// A host-language input (`nvcc -c foo.cpp`) compiles in a single host
    /// step with no preprocess stage: the first pipeline command is the
    /// host compile line. Its defines are kept unfiltered — outside CUDA
    /// mode clang derives none of them (nvcc injects even
    /// __CUDA_ARCH_LIST__ there).
    if(info.host_compiler.empty() && !first_command.empty()) {
        info.host_compiler = first_command[0];
        for(llvm::StringRef token: llvm::ArrayRef(first_command).drop_front()) {
            if(token.consume_front("-D"))
                info.host_defines.emplace_back(token);
        }
        harvest_include_dirs(llvm::ArrayRef(first_command).drop_front());
    }

    if(info.host_compiler.empty())
        return std::unexpected("nvcc dryrun output has no host compile command");

    return info;
}

async::task<std::expected<Job, std::string>> ask_nvcc(llvm::ArrayRef<std::string> args,
                                                      const Command& command,
                                                      llvm::StringRef input,
                                                      llvm::StringRef kind,
                                                      std::string cwd) {
    /// nvcc only offloads CUDA-language inputs; a host-language entry
    /// (`nvcc -c foo.cpp`, or an explicit `-x c++`) compiles in a single
    /// host step. The dryrun query follows the effective language so it
    /// reports the pipeline the real file gets.
    bool cuda_input = kind == "cuda";
    bool cl_host = cl_mode(command.host);
    std::vector<std::string> dryrun_args = {args[0], "--dryrun", "-c", input.str()};
    llvm::StringRef ccbin;
    for(llvm::StringRef arg: args) {
        auto option = nvcc_option(arg);
        if(!option || !is_query_option(option->id)) {
            continue;
        }
        dryrun_args.push_back(arg.str());
        if(option->id == nvcc_options::OPT_compiler_bindir) {
            ccbin = option->values[0];
        }
    }
    std::string path;
    if(cl_host) {
        path = cl_search_path(ccbin);
    }
    auto dryrun = co_await execute(std::move(dryrun_args), cwd, std::move(path));
    if(!dryrun) {
        co_return std::unexpected(std::move(dryrun.error()));
    }
    if(!dryrun->failure.empty()) {
        co_return std::unexpected(std::move(dryrun->failure));
    }
    // nvcc prints its dryrun to stderr, on Windows to stdout.
    auto info = parse_nvcc_dryrun(dryrun->out + dryrun->err);
    if(!info) {
        co_return std::unexpected(std::move(info.error()));
    }

    auto host = find_nvcc_host(*info, args[0]);
    if(!host) {
        co_return std::unexpected(std::move(host.error()));
    }
    /// nvcc hands its host compiler a CUDA input as C++; a GCC-style host is
    /// asked how it compiles that, its headers following the sysroot the
    /// command gives it (`-Xcompiler --sysroot=<dir>`). Clang's own driver
    /// knows cl's: in its cl mode, it takes those of the cl nvcc runs
    /// (cl_driver_mode).
    std::optional<GCCStyleOutput> host_output;
    if(!cl_host) {
        std::vector<std::string> host_query = {*host, "-x", cuda_input ? "c++" : kind.str()};
        for(auto& arg: command.args) {
            if(arg.opt_id == clang_options::OPT__sysroot_EQ) {
                host_query.push_back(std::format("--sysroot={}", arg.values[0]));
            }
        }
        host_query.push_back(input.str());
        auto output = co_await run_gcc_style(host_query, 1, cwd);
        if(!output) {
            co_return std::unexpected(std::move(output.error()));
        }
        if(output->clang_job.empty() && output->gcc_job.empty()) {
            co_return std::unexpected(std::format("Unsupported nvcc host compiler: {}", *host));
        }
        host_output = std::move(*output);
    }

    /// clang's options cl's mode does not read go through `/clang:`.
    auto clang_option = [&](std::string option) {
        return cl_host ? "/clang:" + option : option;
    };
    std::vector<std::string> cuda_args = {*host};
    if(!cl_host) {
        cuda_args.push_back("-fno-delayed-template-parsing");
    }
    if(cuda_input) {
        /// Without a root from the dryrun, clang's own CUDA detection (which
        /// knows the distro-specific install locations) is the remaining
        /// chance.
        if(!info->cuda_path.empty()) {
            cuda_args.push_back(clang_option("--cuda-path=" + info->cuda_path));
        }
        /// A toolkit newer than the linked clang still parses, modulo
        /// missing feature macros; without this the version warning would
        /// land in every file's diagnostics.
        cuda_args.push_back(clang_option("--no-cuda-version-check"));

        /// Default to the device-side view: reading CUDA code means reading
        /// the `__CUDA_ARCH__` world — CUTLASS keeps every arch-specific
        /// path behind it, and the host pass grays them all out.
        /// `--cuda-host-only` in the command (e.g. a config rule append)
        /// flips a file to the host view instead; the last selector wins,
        /// matching the driver's own reading of the forwarded flags.
        std::optional<bool> selected_host;
        for(auto& arg: command.args) {
            if(arg.opt_id == clang_options::OPT_offload_host_only) {
                selected_host = true;
            } else if(arg.opt_id == clang_options::OPT_offload_device_only) {
                selected_host = false;
            }
        }
        if(!selected_host) {
            cuda_args.push_back(clang_option("--cuda-device-only"));
        }
        for(auto& define:
            selected_host.value_or(false) ? info->host_defines : info->device_defines) {
            cuda_args.push_back("-D" + define);
        }

        /// A cl host's `/std:`, which clang's cl mode reads alike, names
        /// one too.
        bool named = names_standard(command.args) ||
                     ranges::contains(command.args, clang_options::OPT__SLASH_std, &Arg::opt_id);
        if(!named && !info->cpp_dialect.empty()) {
            cuda_args.push_back(clang_option("-std=" + info->cpp_dialect));
        }

        /// An `-arch=` query token is an explicit selection too: its
        /// resolution replaces it in place below.
        bool selects_arch = ranges::any_of(command.args, [](const Arg& arg) {
            if(arg.opt_id == clang_options::OPT_offload_arch_EQ) {
                return true;
            }
            auto option =
                arg.opt_id == clang_options::OPT_UNKNOWN ? nvcc_option(arg.spelling) : std::nullopt;
            return option && option->id == nvcc_options::OPT_gpu_architecture;
        });
        if(!selects_arch && !info->default_arch.empty()) {
            cuda_args.push_back(clang_option("--cuda-gpu-arch=" + info->default_arch));
        }
    } else {
        /// The single host step still carries nvcc's identity macros
        /// (__NVCC__, version macros) — but no CUDA mode.
        for(auto& define: info->host_defines) {
            cuda_args.push_back("-D" + define);
        }
    }
    /// What nvcc gives cl ahead of the command's own host flags: C++
    /// exceptions, and the version of cl itself, which clang would
    /// otherwise take from the newest toolset installed.
    if(cl_host) {
        cuda_args.push_back("/EHsc");
        if(!info->ms_version.empty()) {
            cuda_args.push_back("-fms-compatibility-version=" + info->ms_version);
        }
    }

    for(llvm::StringRef arg: flags_for_clang(args.drop_front(), option_family(command))) {
        auto option = nvcc_option(arg);
        if(option && is_query_option(option->id)) {
            /// The dryrun's resolution of an `-arch=<special>` token lands
            /// exactly where the token sat: an edit-appended selection
            /// carries `--no-offload-arch=all` right before it, and
            /// inserting the resolution any earlier would put it on the
            /// cleared side.
            if(cuda_input && option->id == nvcc_options::OPT_gpu_architecture &&
               !info->default_arch.empty()) {
                cuda_args.push_back(clang_option("--cuda-gpu-arch=" + info->default_arch));
            }
            continue;
        }
        cuda_args.push_back(arg.str());
    }

    /// A clang host needs no setup: clang's own driver already defaults to
    /// the running machine's triple.
    std::expected<Job, std::string> job;
    if(cl_host) {
        job = ask_clang(cuda_args, cl_driver_mode(*host, command.host), cwd);
    } else if(!host_output->gcc_job.empty()) {
        job = query_as_gcc(*host_output, cuda_args, cwd);
    } else {
        job = query_driver(cuda_args, cwd);
    }
    if(!job) {
        co_return job;
    }
    /// After the command's own directories and before the host's, as on
    /// nvcc's host line.
    auto host_dirs = ranges::find_if(job->args, [](llvm::StringRef arg) {
        return arg.starts_with("-internal-");
    });
    std::vector<std::string> nvcc_dirs;
    for(auto& dir: info->include_dirs) {
        nvcc_dirs.insert(nvcc_dirs.end(), {"-internal-isystem", dir});
    }
    job->args.insert(host_dirs, nvcc_dirs.begin(), nvcc_dirs.end());
    co_return job;
}

}  // namespace clice

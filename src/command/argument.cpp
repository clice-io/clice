module clice;

import :command.argument;
import :vfs.path;

namespace clice {

namespace clang_options {

namespace detail {

#define OPTTABLE_STR_TABLE_CODE
#include "clang/Options/Options.inc"
#undef OPTTABLE_STR_TABLE_CODE

#define OPTTABLE_PREFIXES_TABLE_CODE
#include "clang/Options/Options.inc"
#undef OPTTABLE_PREFIXES_TABLE_CODE

static_assert(OptionPrefixesTable[0].value() == 0, "pfx_none: 0 prefixes");
static_assert(OptionPrefixesTable[1].value() == 1, "pfx_dash: 1 prefix");
static_assert(OptionPrefixesTable[3].value() == 2, "pfx_dash_double: 2 prefixes");
static_assert(OptionPrefixesTable[6].value() == 1, "pfx_double: 1 prefix");
static_assert(OptionPrefixesTable[8].value() == 3, "pfx_all: 3 prefixes");
static_assert(OptionPrefixesTable[12].value() == 2, "pfx_slash_dash: 2 prefixes");

constexpr std::span<const std::string_view> prefixes(unsigned offset) {
    switch(offset) {
        case 0: return cli::pfx_none;
        case 1: return cli::pfx_dash;
        case 3: return cli::pfx_dash_double;
        case 6: return cli::pfx_double;
        case 8: return cli::pfx_all;
        case 12: return cli::pfx_slash_dash;
        default: std::unreachable();
    }
}

constexpr std::string_view str_at(unsigned offset) {
    auto ref = OptionStrTable[llvm::StringTable::Offset(offset)];
    return {ref.data(), ref.size()};
}

/// Plain enum: Options.inc combines these with bitwise-or, which needs the
/// implicit conversion an enum class forbids.
enum ClangDriverFlag : unsigned {
    HelpHidden = cli::HelpHidden,
    RenderAsInput = cli::RenderAsInput,
    RenderJoined = cli::RenderJoined,
    Ignored = 1u << 4,
    LinkOption = 1u << 5,
    LinkerInput = 1u << 6,
    NoArgumentUnused = 1u << 7,
    NoXarchOption = 1u << 8,
    TargetSpecific = 1u << 9,
    Unsupported = 1u << 10,
};

}  // namespace detail

const cli::OptTable& table() {
    using enum cli::OptionKind;
    using enum detail::ClangDriverFlag;
    using detail::prefixes;
    using detail::str_at;

    constexpr static cli::Option option_infos[] = {
#define OPTION(PREFIXES_OFFSET,                                                                    \
               NAME_OFFSET,                                                                        \
               ID,                                                                                 \
               KIND,                                                                               \
               GROUP,                                                                              \
               ALIAS,                                                                              \
               ALIAS_ARGS,                                                                         \
               FLAGS,                                                                              \
               VISIBILITY,                                                                         \
               PARAM,                                                                              \
               HELP,                                                                               \
               HELP_TEXTS,                                                                         \
               META_VAR,                                                                           \
               VALUES,                                                                             \
               SUBCOMMAND_IDS_OFFSET)                                                              \
    cli::Option{                                                                                   \
        .prefixes = prefixes(PREFIXES_OFFSET),                                                     \
        .prefixed_name = str_at(NAME_OFFSET),                                                      \
        .id = OPT_##ID,                                                                            \
        .kind = KIND,                                                                              \
        .group_id = OPT_##GROUP,                                                                   \
        .alias_id = OPT_##ALIAS,                                                                   \
        .alias_args = ALIAS_ARGS,                                                                  \
        .flags = FLAGS,                                                                            \
        .visibility = VISIBILITY,                                                                  \
        .num_args = PARAM,                                                                         \
        .help_text = HELP,                                                                         \
        .meta_var = META_VAR,                                                                      \
    },
#include "clang/Options/Options.inc"
#undef OPTION
    };

    const static auto opt_table = [] {
        auto t = cli::OptTable(std::span(option_infos));
        t.tablegen_mode = true;
        return t;
    }();
    return opt_table;
}

}  // namespace clang_options

using namespace clang_options;

CompilerFamily driver_family(llvm::StringRef driver) {
    auto try_get = [](llvm::StringRef name) {
        if(name == "cl")
            return CompilerFamily::MSVC;
        if(name == "nvcc")
            return CompilerFamily::NVCC;
        // Intel's -cl drivers read cl's options too, and so does its icx
        // on Windows, which Intel's own driver runs in cl mode there.
        if(name.ends_with("clang-cl") || name.contains("icx-cl") || name.contains("dpcpp-cl"))
            return CompilerFamily::ClangCL;
#ifdef _WIN32
        if(name == "icx" || name == "icx.exe")
            return CompilerFamily::ClangCL;
#endif
        if(name.ends_with("clang") || name.ends_with("clang++"))
            return CompilerFamily::Clang;
        // Intel must precede GCC: `icc` would otherwise match ends_with("cc").
        if(name.contains("icpc") || name.contains("icc") || name.contains("dpcpp") ||
           name.contains("icx") || name.contains("icpx"))
            return CompilerFamily::Intel;
        if(name.ends_with("cc") || name.ends_with("c++") || name.ends_with("gcc") ||
           name.ends_with("g++"))
            return CompilerFamily::GCC;
        if(name.ends_with("zig"))
            return CompilerFamily::Zig;
        return CompilerFamily::Unknown;
    };

    /// Windows resolves executable names case-insensitively, and CDBs record
    /// spellings like CL.exe or Clang-Cl.EXE — match lowercased.
    std::string lowered = llvm::StringRef(path::filename(driver)).lower();
    llvm::StringRef name = lowered;
    if(auto f = try_get(name); f != CompilerFamily::Unknown)
        return f;

    // Stripping the executable suffix: clang++.exe -> clang++
    name.consume_back(".exe");
    if(auto f = try_get(name); f != CompilerFamily::Unknown)
        return f;

    // Stripping any trailing version number: clang++3.5 -> clang++
    name = name.rtrim("0123456789.-");
    if(auto f = try_get(name); f != CompilerFamily::Unknown)
        return f;

    // Stripping trailing -component: clang++-tot -> clang++
    name = name.slice(0, name.rfind('-'));
    return try_get(name);
}

std::string name_target(llvm::StringRef driver) {
    auto name = clang::driver::ToolChain::getTargetAndModeFromProgramName(driver);
    // clang takes the prefix only when a backend it links knows it, and
    // clice links none: any prefix naming an architecture is a target.
    if(llvm::Triple(name.TargetPrefix).getArch() == llvm::Triple::UnknownArch) {
        return {};
    }
    return name.TargetPrefix;
}

unsigned family_visibility(CompilerFamily family) {
    /// Flang's own options shadow clang's: -Wextra and -Wconversion have
    /// flang-only spellings that flang ignores, matched before -W<warning>.
    auto flang = static_cast<unsigned>(clang_options::FlangOption | clang_options::FC1Option);
    /// Exclude the slash-prefixed CL and DXC options otherwise (/D and /I
    /// carry both bits), to prevent /U, /D, /I from matching Unix absolute
    /// paths like /Users/... .
    if(cl_mode(family)) {
        return ~flang;
    }
    return ~(flang | clang_options::CLOption | clang_options::DXCOption);
}

llvm::SmallVector<std::size_t> forced_files(llvm::ArrayRef<Arg> args) {
    llvm::SmallVector<std::size_t> positions;
    for(auto id: {OPT_imacros, OPT_include}) {
        for(auto [position, arg]: views::enumerate(args)) {
            if(arg.opt_id == id) {
                positions.push_back(position);
            }
        }
    }
    return positions;
}

bool names_path(unsigned id, llvm::StringRef value) {
    if(value.empty() || value.starts_with("=") || value.starts_with("$SYSROOT")) {
        return false;
    }
    switch(id) {
        case OPT_o:
        case OPT_I:
        case OPT_isystem:
        case OPT_iquote:
        case OPT_idirafter:
        case OPT_isystem_after:
        case OPT_c_isystem:
        case OPT_cxx_isystem:
        case OPT_objc_isystem:
        case OPT_objcxx_isystem:
        case OPT_stdlibxx_isystem:
        case OPT_F:
        case OPT_iframework:
        case OPT_isysroot:
        case OPT__sysroot_EQ:
        case OPT__SLASH_imsvc:
        case OPT__SLASH_winsysroot:
        case OPT_B:
        case OPT_gcc_toolchain:
        case OPT_gcc_install_dir_EQ:
        case OPT_cuda_path_EQ:
        case OPT_hip_path_EQ:
        case OPT_rocm_path_EQ:
        case OPT_rocm_device_lib_path_EQ:
        case OPT_resource_dir:
        case OPT_config_system_dir_EQ:
        case OPT_config_user_dir_EQ:
        case OPT_ivfsoverlay:
        case OPT_fsanitize_ignorelist_EQ:
        case OPT_fsanitize_system_ignorelist_EQ:
        case OPT_fprofile_instr_use_EQ:
        case OPT_fprofile_use_EQ:
        case OPT_include_pch:
        case OPT__SLASH_Fp:
        case OPT_working_directory: return true;
        case OPT_config: return value.contains('/') || value.contains('\\');
        default: return false;
    }
}

bool is_xclang_option(unsigned id) {
    return id == OPT_Xclang;
}

unsigned effective_option(const Arg& arg) {
    if(arg.opt_id != OPT__SLASH_clang && arg.opt_id != OPT_Xclang) {
        return arg.opt_id;
    }
    std::array passed = {std::string_view(arg.values[0])};
    auto visibility =
        arg.opt_id == OPT_Xclang ? unsigned(CC1Option) : family_visibility(CompilerFamily::Clang);
    auto parsed = *clang_options::table().parse(passed, {.visibility = visibility}).begin();
    return parsed ? parsed->id : OPT_UNKNOWN;
}

/// Whether `args` hold dependency file options alone. The preprocessor's -MD
/// and -MMD take the file they write as their argument, which the table
/// reads as an input.
static bool dependency_options(llvm::ArrayRef<const char*> args) {
    auto options = cli::ParseOptions{.visibility = family_visibility(CompilerFamily::GCC)};
    bool file_next = false;
    for(auto& parsed: clang_options::table().parse(args, options)) {
        if(!parsed) {
            return false;
        }
        if(file_next && parsed->id == clang_options::OPT_INPUT) {
            file_next = false;
            continue;
        }
        if(auto opt = clang_options::table().option(parsed->id);
           !opt || !opt->matches(clang_options::OPT_M_Group)) {
            return false;
        }
        file_next = parsed->id == clang_options::OPT_MD || parsed->id == clang_options::OPT_MMD;
    }
    return true;
}

/// The class of each of clang's options by id, none for one a command leaves
/// out (option_classes.inc).
const static std::vector<std::optional<ArgClass>>& option_classes() {
    const static auto classes = [] {
        using enum ArgClass;
        constexpr std::optional<ArgClass> LeftOut;
        auto& table = clang_options::table();
        std::vector<std::optional<ArgClass>> classes(table.option_infos.size() + 1, Semantic);
        for(auto& option: table.option_infos) {
            if(table.option(option.id)->has_flag(clang_options::detail::Ignored)) {
                classes[option.id] = LeftOut;
            }
        }
#define CODEGEN_OPTION_WITH_MARSHALLING(PREFIX_TYPE, SPELLING_OFFSET, ID, ...)                     \
    classes[OPT_##ID] = LeftOut;
#include "clang/Options/Options.inc"
#undef CODEGEN_OPTION_WITH_MARSHALLING
        auto group = [&](unsigned id, std::optional<ArgClass> cls) {
            for(auto& option: table.option_infos) {
                if(table.option(option.id)->matches(id)) {
                    classes[option.id] = cls;
                }
            }
        };
#define OPTION_GROUP(GROUP, CLASS) group(OPT_##GROUP, CLASS);
#define OPTION(ID, CLASS) classes[OPT_##ID] = CLASS;
#include "command/option_classes.inc"
#undef OPTION
#undef OPTION_GROUP
        return classes;
    }();
    return classes;
}

std::optional<ArgClass> classify_option(unsigned id, llvm::ArrayRef<const char*> values) {
    /// -fmodule-file has two shapes: `name=path` names a prebuilt module
    /// (clice builds its own PCMs — irrelevant), a bare path is a header
    /// unit that stays part of the frontend semantics.
    if(id == clang_options::OPT_fmodule_file) {
        bool named = values.size() == 1 && llvm::StringRef(values[0]).contains('=');
        return named ? std::nullopt : std::optional(ArgClass::Semantic);
    }
    /// Kbuild hands the preprocessor `-Wp,-MMD,<depfile>` in every command:
    /// it goes as `-MMD -MF <depfile>` does, or each command would key a
    /// query of its own.
    if(id == clang_options::OPT_Wp_COMMA && dependency_options(values)) {
        return std::nullopt;
    }
    /// g++ takes an LTO job count (-flto=8), which clang rejects.
    if(id == clang_options::OPT_flto_EQ && values.size() == 1 &&
       !ranges::contains(std::array{"full", "thin", "none"}, llvm::StringRef(values[0]))) {
        return std::nullopt;
    }
    return option_classes()[id];
}

std::uint16_t written_alias(const cli::ParsedArg& parsed) {
    auto& options = clang_options::table().option_infos;
    auto written = [&](std::uint32_t id) {
        auto& option = options[id - 1];
        return ranges::any_of(option.prefixes, [&](std::string_view prefix) {
            return parsed.spelling.size() == prefix.size() + option.name().size() &&
                   parsed.spelling.starts_with(prefix) && parsed.spelling.ends_with(option.name());
        });
    };
    if(written(parsed.id)) {
        return 0;
    }
    const static auto aliases = [&] {
        llvm::DenseMap<std::uint32_t, llvm::SmallVector<std::uint16_t, 2>> aliases;
        for(auto& option: options) {
            if(option.alias_id) {
                aliases[option.alias_id].push_back(option.id);
            }
        }
        return aliases;
    }();
    auto& candidates = aliases.find(parsed.id)->second;
    auto alias = ranges::find_if(candidates, written);
    assert(alias != candidates.end() && "the parse matched the option or an alias of it");
    return *alias;
}

void render_arg(const Arg& arg, llvm::function_ref<void(std::string_view)> cb) {
    if(arg.opt_id == clang_options::OPT_UNKNOWN) {
        cb(arg.spelling);
        for(const char* value: arg.values) {
            cb(value);
        }
        return;
    }
    cli::ParsedArg parsed;
    parsed.id = arg.opt_id;
    for(const char* value: arg.values) {
        parsed.add_value(value);
    }
    auto forward = [&](std::string_view fragment) {
        cb(fragment);
    };
    clang_options::table().render(parsed, forward);
}

static void render_alias(const Arg& arg, llvm::function_ref<void(std::string_view)> cb) {
    auto alias = clang_options::table().option(arg.alias);
    std::string name(alias->prefixed_name());
    // A flag alias implies its values (`-flto` is -flto=full).
    if(alias->kind() == cli::OptionKind::Flag) {
        cb(name);
        return;
    }
    switch(alias->render_style()) {
        case cli::RenderStyle::Joined:
            cb(name + arg.values[0]);
            for(const char* value: arg.values.drop_front()) {
                cb(value);
            }
            return;
        case cli::RenderStyle::Separate:
            cb(name);
            for(const char* value: arg.values) {
                cb(value);
            }
            return;
        case cli::RenderStyle::CommaJoined:
            for(auto [i, value]: views::enumerate(arg.values)) {
                name += i == 0 ? "" : ",";
                name += value;
            }
            cb(name);
            return;
        case cli::RenderStyle::Values: std::unreachable();
    }
}

bool keeps_alias(const Arg& arg, CompilerFamily family) {
    // nvcc never runs its arguments, clice's translation of its command:
    // its query hands them to clang's driver, which knows every alias.
    return arg.alias && !cl_mode(family) && family != CompilerFamily::NVCC &&
           clang_options::table().option(arg.alias)->has_visibility_flag(family_visibility(family));
}

void render_driver_arg(const Arg& arg,
                       CompilerFamily family,
                       llvm::function_ref<void(std::string_view)> cb) {
    if(keeps_alias(arg, family)) {
        render_alias(arg, cb);
        return;
    }
    if(!cl_mode(family)) {
        render_arg(arg, cb);
        return;
    }

    auto reads_back = [&](std::vector<std::string>& fragments) {
        auto parse_options = cli::ParseOptions{.visibility = clang_options::CLOption};
        std::size_t count = 0;
        bool same = true;
        for(auto& parsed: clang_options::table().parse(fragments, parse_options)) {
            count += 1;
            same = same && parsed && parsed->id == arg.opt_id &&
                   ranges::equal(parsed->values,
                                 arg.values,
                                 [](std::string_view lhs, const char* rhs) { return lhs == rhs; });
        }
        return same && count == 1;
    };
    auto emit = [&](llvm::ArrayRef<std::string> fragments) {
        for(auto& fragment: fragments) {
            cb(fragment);
        }
    };

    std::vector<std::string> fragments;
    render_arg(arg, [&](std::string_view fragment) { fragments.emplace_back(fragment); });
    if(reads_back(fragments)) {
        emit(fragments);
        return;
    }

    // A cl spelling of the option keeps its place among the arguments,
    // where order decides (`/W3 -Wno-unused-variable`).
    for(auto& option: clang_options::table().option_infos) {
        if(option.alias_id != arg.opt_id || !(option.visibility & clang_options::CLOption)) {
            continue;
        }
        std::vector<std::string> spelled{std::string(option.prefixed_name)};
        if(option.kind != cli::OptionKind::Flag) {
            if(arg.values.size() != 1) {
                continue;
            }
            spelled[0] += arg.values[0];
        }
        if(reads_back(spelled)) {
            emit(spelled);
            return;
        }
    }

    // The driver appends `/clang:` arguments after all others.
    for(auto& fragment: fragments) {
        cb("/clang:" + fragment);
    }
}

std::string canonicalize(llvm::ArrayRef<std::string> args) {
    std::string buf;
    if(args.empty()) {
        return buf;
    }

    llvm::raw_string_ostream os(buf);
    auto append = [&](std::string_view fragment) {
        os << fragment << '\0';
    };

    /// The driver affects language defaults (clang vs clang++ vs clang-cl).
    append(args[0]);

    std::vector<std::string> parse_args(args.begin() + 1, args.end());
    auto options = cli::ParseOptions{.dash_dash_parsing = true,
                                     .visibility = family_visibility(driver_family(args[0]))};
    for(auto& result: clang_options::table().parse(parse_args, options)) {
        /// Unparseable and unknown arguments are kept verbatim: dropping an
        /// option we don't understand could merge keys that must differ.
        if(!result || result->id == OPT_UNKNOWN) {
            append(parse_args[result ? result->index : result.error().begin]);
            continue;
        }
        std::vector<std::string> storage(result->values.begin(), result->values.end());
        llvm::SmallVector<const char*, 2> values;
        for(auto& value: storage) {
            values.push_back(value.c_str());
        }
        if(auto cls = classify_option(result->id, values); cls && cls != ArgClass::Precompiled) {
            clang_options::table().render(*result, append);
        }
    }

    return buf;
}

}  // namespace clice

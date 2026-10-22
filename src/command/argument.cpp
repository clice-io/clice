module;

#include "modules/prelude.h"

module clice;

import :command.argument;

namespace clice {

namespace option {

namespace eo = kota::option;

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
        case 0: return eo::pfx_none;
        case 1: return eo::pfx_dash;
        case 3: return eo::pfx_dash_double;
        case 6: return eo::pfx_double;
        case 8: return eo::pfx_all;
        case 12: return eo::pfx_slash_dash;
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
    HelpHidden = eo::HelpHidden,
    RenderAsInput = eo::RenderAsInput,
    RenderJoined = eo::RenderJoined,
    Ignored = 1u << 4,
    LinkOption = 1u << 5,
    LinkerInput = 1u << 6,
    NoArgumentUnused = 1u << 7,
    NoXarchOption = 1u << 8,
    TargetSpecific = 1u << 9,
    Unsupported = 1u << 10,
};

}  // namespace detail

const eo::OptTable& table() {
    using enum eo::Kind;
    using enum detail::ClangDriverFlag;
    using detail::prefixes;
    using detail::str_at;

    constexpr static eo::Option option_infos[] = {
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
    eo::Option{                                                                                    \
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
        auto t = eo::OptTable(std::span(option_infos));
        t.tablegen_mode = true;
        return t;
    }();
    return opt_table;
}

}  // namespace option

using namespace option;

CompilerFamily driver_family(llvm::StringRef driver) {
    auto try_get = [](llvm::StringRef name) {
        if(name == "cl")
            return CompilerFamily::MSVC;
        if(name == "nvcc")
            return CompilerFamily::NVCC;
        if(name.ends_with("clang-cl"))
            return CompilerFamily::ClangCL;
        if(name.ends_with("clang") || name.ends_with("clang++"))
            return CompilerFamily::Clang;
        // Intel must precede GCC: `icc` would otherwise match ends_with("cc").
        if(name.contains("icpc") || name.contains("icc") || name.contains("dpcpp") ||
           name.contains("icx"))
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
    std::string lowered = llvm::sys::path::filename(driver).lower();
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
    auto flang = static_cast<unsigned>(option::FlangOption | option::FC1Option);
    /// Exclude the slash-prefixed CL and DXC options otherwise (/D and /I
    /// carry both bits), to prevent /U, /D, /I from matching Unix absolute
    /// paths like /Users/... .
    if(cl_mode(family)) {
        return ~flang;
    }
    return ~(flang | option::CLOption | option::DXCOption);
}

bool is_discarded_option(unsigned id) {
    /// Options the driver accepts and ignores (gcc compat knobs, e.g. the
    /// per-file -frandom-seed=<output> bazel stamps on every command): a
    /// per-file value here must not fracture query and identity keys.
    /// Deliberately also drops clang's -Wignored-optimization-argument
    /// compat warnings for them — under -Werror those abort the analysis.
    if(auto opt = option::table().option(id); opt && opt->has_flag(option::detail::Ignored)) {
        return true;
    }

    switch(id) {
        /// Input file, unknown args, and output — we manage these ourselves.
        /// -main-file-name is per-file input identity injected by to_argv()
        /// for cc1 commands; it only labels diagnostics/debug info.
        case OPT_INPUT:
        case OPT_UNKNOWN:
        case OPT__DASH_DASH:
        case OPT_main_file_name:
        case OPT_c:
        case OPT_o:
        case OPT_dxc_Fc:
        case OPT_dxc_Fo:
        case OPT__SLASH_Fo:
        case OPT__SLASH_Fd:

        /// PCH building.
        case OPT_emit_pch:
        case OPT_include_pch:
        case OPT__SLASH_Yc:
        case OPT__SLASH_Yu:
        case OPT__SLASH_Fp:

        /// Dependency scan.
        case OPT_E:
        case OPT_M:
        case OPT_MM:
        case OPT_MD:
        case OPT_MMD:
        case OPT_MF:
        case OPT_MT:
        case OPT_MQ:
        case OPT_MG:
        case OPT_MP:
        case OPT_show_inst:
        case OPT_show_encoding:
        case OPT_show_includes:
        case OPT__SLASH_showFilenames:
        case OPT__SLASH_showFilenames_:
        case OPT__SLASH_showIncludes:
        case OPT__SLASH_showIncludes_user:

        /// C++ modules — we handle these ourselves.
        case OPT_fmodule_file:
        case OPT_fmodule_output:
        case OPT_fprebuilt_module_path: return true;

        default: return false;
    }
}

bool is_user_content_option(unsigned id) {
    switch(id) {
        case OPT_I:
        case OPT_isystem:
        case OPT_iquote:
        case OPT_idirafter:
        case OPT_D:
        case OPT_U:
        case OPT_include:
        case OPT_imacros: return true;
        default: return false;
    }
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
        case OPT_cxx_isystem:
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
        case OPT_fprebuilt_module_path:
        case OPT_fmodules_cache_path:
        case OPT_fmodule_map_file:
        case OPT_ivfsoverlay:
        case OPT_fsanitize_ignorelist_EQ:
        case OPT_fsanitize_system_ignorelist_EQ:
        case OPT_fsanitize_coverage_allowlist:
        case OPT_fsanitize_coverage_ignorelist:
        case OPT_fprofile_list_EQ:
        case OPT_fxray_attr_list:
        case OPT_fxray_always_instrument:
        case OPT_fxray_never_instrument:
        case OPT_fprofile_instr_use_EQ:
        case OPT_fprofile_use_EQ:
        case OPT_fprofile_sample_use_EQ:
        case OPT_fprofile_remapping_file_EQ:
        case OPT_fbuild_session_file:
        case OPT_include_pch:
        case OPT_working_directory: return true;
        case OPT_config: return value.contains('/') || value.contains('\\');
        default: return false;
    }
}

bool is_xclang_option(unsigned id) {
    return id == OPT_Xclang;
}

/// Whether `args` hold dependency file options alone. The preprocessor's -MD
/// and -MMD take the file they write as their argument, which the table
/// reads as an input.
static bool dependency_options(llvm::ArrayRef<const char*> args) {
    auto options = kota::option::ParseOptions{.visibility = family_visibility(CompilerFamily::GCC)};
    bool file_next = false;
    for(auto& parsed: option::table().parse(args, options)) {
        if(!parsed) {
            return false;
        }
        if(file_next && parsed->id == option::OPT_INPUT) {
            file_next = false;
            continue;
        }
        if(auto opt = option::table().option(parsed->id);
           !opt || !opt->matches(option::OPT_M_Group)) {
            return false;
        }
        file_next = parsed->id == option::OPT_MD || parsed->id == option::OPT_MMD;
    }
    return true;
}

ArgClass classify_option(unsigned id, llvm::ArrayRef<const char*> values) {
    /// -fmodule-file has two shapes: `name=path` names a prebuilt module
    /// (clice builds its own PCMs — irrelevant), a bare path is a header
    /// unit that stays part of the frontend semantics.
    if(id == option::OPT_fmodule_file) {
        bool named = values.size() == 1 && llvm::StringRef(values[0]).contains('=');
        return named ? ArgClass::Discarded : ArgClass::Semantic;
    }
    /// Kbuild hands the preprocessor `-Wp,-MMD,<depfile>` in every command:
    /// it goes as `-MMD -MF <depfile>` does, or each command would key a
    /// query of its own.
    if(id == option::OPT_Wp_COMMA && dependency_options(values)) {
        return ArgClass::Discarded;
    }
    /// g++ takes an LTO job count (-flto=8), which clang rejects.
    if(id == option::OPT_flto_EQ && values.size() == 1 &&
       !std::ranges::contains(std::array{"full", "thin", "none"}, llvm::StringRef(values[0]))) {
        return ArgClass::Discarded;
    }
    if(is_discarded_option(id)) {
        return ArgClass::Discarded;
    }
    if(is_codegen_option(id)) {
        return ArgClass::Codegen;
    }
    if(is_user_content_option(id)) {
        return ArgClass::UserContent;
    }
    if(is_diagnostics_option(id)) {
        return ArgClass::Diagnostics;
    }
    return ArgClass::Semantic;
}

bool is_codegen_option(unsigned id) {
    /// Debug info options form a group (-g, -gdwarf-*, -gsplit-dwarf, etc.).
    if(auto opt = option::table().option(id); opt && opt->matches(OPT_DebugInfo_Group)) {
        return true;
    }

    switch(id) {
        /// Frame pointer and unwind tables — pure codegen.
        case OPT_mframe_pointer_EQ:
        case OPT_fomit_frame_pointer:
        case OPT_fno_omit_frame_pointer:
        case OPT_funwind_tables:
        case OPT_fno_unwind_tables:
        case OPT_fasynchronous_unwind_tables:
        case OPT_fno_asynchronous_unwind_tables:

        /// Section splitting, semantic interposition — pure codegen/linker.
        /// Not LTO: the driver allows -fsanitize=cfi and
        /// -fwhole-program-vtables only with it.
        case OPT_fdata_sections:
        case OPT_fno_data_sections:
        case OPT_ffunction_sections:
        case OPT_fno_function_sections:
        case OPT_fsemantic_interposition:
        case OPT_fno_semantic_interposition:
        case OPT_fvisibility_inlines_hidden:

        /// Diagnostics output formatting — doesn't affect analysis.
        case OPT_fcolor_diagnostics:
        case OPT_fno_color_diagnostics:
        case OPT_fdiagnostics_color_EQ:

        /// Floating-point codegen — doesn't define macros (unlike -ffast-math).
        case OPT_ftrapping_math:
        case OPT_fno_trapping_math: return true;

        default: return false;
    }
}

bool is_diagnostics_option(unsigned id) {
    if(id == OPT_w) {
        return true;
    }
    if(auto opt = option::table().option(id)) {
        return opt->matches(OPT_Diag_Group) || opt->matches(OPT_pedantic_Group);
    }
    return false;
}

std::uint16_t written_alias(const kota::option::ParsedArg& parsed) {
    auto& options = option::table().option_infos;
    auto written = [&](std::uint32_t id) {
        auto& option = options[id - 1];
        return std::ranges::any_of(option.prefixes, [&](std::string_view prefix) {
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
    auto alias = std::ranges::find_if(candidates, written);
    assert(alias != candidates.end() && "the parse matched the option or an alias of it");
    return *alias;
}

void render_arg(const Arg& arg, llvm::function_ref<void(std::string_view)> cb) {
    if(arg.opt_id == option::OPT_UNKNOWN) {
        cb(arg.spelling);
        for(const char* value: arg.values) {
            cb(value);
        }
        return;
    }
    kota::option::ParsedArg parsed;
    parsed.id = arg.opt_id;
    for(const char* value: arg.values) {
        parsed.add_value(value);
    }
    auto forward = [&](std::string_view fragment) {
        cb(fragment);
    };
    option::table().render(parsed, forward);
}

static void render_alias(const Arg& arg, llvm::function_ref<void(std::string_view)> cb) {
    auto alias = option::table().option(arg.alias);
    std::string name(alias->prefixed_name());
    // A flag alias implies its values (`-flto` is -flto=full).
    if(alias->kind() == kota::option::Kind::Flag) {
        cb(name);
        return;
    }
    switch(alias->render_style()) {
        case kota::option::RenderStyle::Joined:
            cb(name + arg.values[0]);
            for(const char* value: arg.values.drop_front()) {
                cb(value);
            }
            return;
        case kota::option::RenderStyle::Separate:
            cb(name);
            for(const char* value: arg.values) {
                cb(value);
            }
            return;
        case kota::option::RenderStyle::CommaJoined:
            for(auto [i, value]: std::views::enumerate(arg.values)) {
                name += i == 0 ? "" : ",";
                name += value;
            }
            cb(name);
            return;
        case kota::option::RenderStyle::Values: std::unreachable();
    }
}

bool keeps_alias(const Arg& arg, CompilerFamily family) {
    // nvcc's arguments are clice's translation of its command, which its
    // query reads by clang's names.
    return arg.alias && !cl_mode(family) && family != CompilerFamily::NVCC &&
           option::table().option(arg.alias)->has_visibility_flag(family_visibility(family));
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
        auto parse_options = kota::option::ParseOptions{.visibility = option::CLOption};
        std::size_t count = 0;
        bool same = true;
        for(auto& parsed: option::table().parse(fragments, parse_options)) {
            count += 1;
            same = same && parsed && parsed->id == arg.opt_id &&
                   std::ranges::equal(
                       parsed->values,
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
    for(auto& option: option::table().option_infos) {
        if(option.alias_id != arg.opt_id || !(option.visibility & option::CLOption)) {
            continue;
        }
        std::vector<std::string> spelled{std::string(option.prefixed_name)};
        if(option.kind != kota::option::Kind::Flag) {
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
    auto options =
        kota::option::ParseOptions{.dash_dash_parsing = true,
                                   .visibility = family_visibility(driver_family(args[0]))};
    for(auto& result: option::table().parse(parse_args, options)) {
        /// Unparseable and unknown arguments are kept verbatim: dropping an
        /// option we don't understand could merge keys that must differ.
        if(!result || result->id == OPT_UNKNOWN) {
            append(parse_args[result ? result->index : result.error().index]);
            continue;
        }
        if(!is_discarded_option(result->id) && !is_codegen_option(result->id)) {
            option::table().render(*result, append);
        }
    }

    return buf;
}

}  // namespace clice

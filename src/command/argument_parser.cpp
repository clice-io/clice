#include "command/argument_parser.h"

#include <span>
#include <string_view>
#include <utility>

#include <kota/deco/option.h>
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringTable.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"
#include "clang/Driver/Driver.h"
#include "clang/Driver/Types.h"

namespace clice {

namespace option {

namespace eo = kota::option;

namespace detail {

#define OPTTABLE_STR_TABLE_CODE
#include "clang/Driver/Options.inc"
#undef OPTTABLE_STR_TABLE_CODE

#define OPTTABLE_PREFIXES_TABLE_CODE
#include "clang/Driver/Options.inc"
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

}  // namespace detail

const eo::OptTable& table() {
    using enum eo::Kind;
    using detail::prefixes;
    using detail::str_at;

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
               VALUES)                                                                             \
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
#include "clang/Driver/Options.inc"
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

bool is_discarded_option(unsigned id) {
    switch(id) {
        /// Input file, unknown args, and output — we manage these ourselves.
        case OPT_INPUT:
        case OPT_UNKNOWN:
        case OPT__DASH_DASH:
        case OPT_c:
        case OPT_o:
        case OPT_dxc_Fc:
        case OPT_dxc_Fo:
        case OPT__SLASH_Fo:
        case OPT__SLASH_Fd:

        /// PCH building.
        case OPT_emit_pch:
        case OPT_include_pch:
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
        case OPT_include: return true;
        default: return false;
    }
}

bool is_include_path_option(unsigned id) {
    switch(id) {
        case OPT_I:
        case OPT_isystem:
        case OPT_iquote:
        case OPT_idirafter: return true;
        default: return false;
    }
}

bool is_xclang_option(unsigned id) {
    return id == OPT_Xclang;
}

bool is_toolchain_option(unsigned id) {
    switch(id) {
        case OPT_target:
        case OPT_target_legacy_spelling:
        case OPT_isysroot:
        case OPT__sysroot_EQ:
        case OPT__sysroot:
        case OPT_stdlib_EQ:
        case OPT_gcc_toolchain:
        case OPT_gcc_install_dir_EQ:
        case OPT_nostdinc:
        case OPT_nostdincxx:
        case OPT_std_EQ:
        case OPT_x: return true;
        default: return false;
    }
}

llvm::StringRef resource_dir() {
    static std::string dir = [] {
        static int anchor;
        auto exe = llvm::sys::fs::getMainExecutable("", &anchor);
        if(exe.empty()) {
            return std::string{};
        }
        return clang::driver::Driver::GetResourcesPath(exe);
    }();
    return dir;
}

bool is_codegen_option(unsigned id) {
    /// Debug info options form a group (-g, -gdwarf-*, -gsplit-dwarf, etc.).
    if(auto opt = option::table().option(id); opt && opt->matches(OPT_DebugInfo_Group)) {
        return true;
    }

    switch(id) {
        /// Position-independent code — pure codegen, no macro or semantic effect.
        case OPT_fPIC:
        case OPT_fno_PIC:
        case OPT_fpic:
        case OPT_fno_pic:
        case OPT_fPIE:
        case OPT_fno_PIE:
        case OPT_fpie:
        case OPT_fno_pie:

        /// Frame pointer and unwind tables — pure codegen.
        case OPT_fomit_frame_pointer:
        case OPT_fno_omit_frame_pointer:
        case OPT_funwind_tables:
        case OPT_fno_unwind_tables:
        case OPT_fasynchronous_unwind_tables:
        case OPT_fno_asynchronous_unwind_tables:

        /// Stack protection — pure codegen.
        case OPT_fstack_protector:
        case OPT_fstack_protector_strong:
        case OPT_fstack_protector_all:
        case OPT_fno_stack_protector:

        /// Section splitting, LTO, semantic interposition — pure codegen/linker.
        case OPT_fdata_sections:
        case OPT_fno_data_sections:
        case OPT_ffunction_sections:
        case OPT_fno_function_sections:
        case OPT_flto:
        case OPT_flto_EQ:
        case OPT_fno_lto:
        case OPT_fsemantic_interposition:
        case OPT_fno_semantic_interposition:
        case OPT_fvisibility_inlines_hidden:

        /// Diagnostics output formatting — doesn't affect analysis.
        case OPT_fcolor_diagnostics:
        case OPT_fno_color_diagnostics:

        /// Floating-point codegen — doesn't define macros (unlike -ffast-math).
        case OPT_ftrapping_math:
        case OPT_fno_trapping_math: return true;

        default: return false;
    }
}

std::string print_argv(llvm::ArrayRef<const char*> args) {
    std::string buf;
    llvm::raw_string_ostream os(buf);
    bool sep = false;
    for(llvm::StringRef arg: args) {
        if(sep)
            os << ' ';
        sep = true;
        if(llvm::all_of(arg, llvm::isPrint) &&
           arg.find_first_of(" \t\n\"\\") == llvm::StringRef::npos) {
            os << arg;
            continue;
        }
        os << '"';
        os.write_escaped(arg, /*UseHexEscapes=*/true);
        os << '"';
    }
    return std::move(os.str());
}

unsigned default_visibility(llvm::StringRef driver) {
    auto name = llvm::sys::path::filename(driver);
    name.consume_back(".exe");

    auto is_cl = [](llvm::StringRef s) {
        return s.equals_insensitive("cl") || s.equals_insensitive("clang-cl");
    };

    /// cl.exe and clang-cl.exe both need MSVC-style /options.
    /// Also handle versioned names like clang-cl-17, clang-cl-17.0.1.
    if(is_cl(name) || is_cl(name.rtrim("0123456789.-"))) {
        return ~0u;
    }
    /// Exclude CLOption to prevent /U, /D, /I from matching Unix paths.
    return ~static_cast<unsigned>(CLOption);
}

bool is_c_family_file(llvm::StringRef filename) {
    namespace types = clang::driver::types;
    auto ext = llvm::sys::path::extension(filename);
    if(ext.empty()) {
        return false;
    }
    /// Drop the leading dot: ".cpp" -> "cpp".
    auto type = types::lookupTypeForExtension(ext.drop_front());
    return type != types::TY_INVALID && types::isAcceptedByClang(type);
}

}  // namespace clice

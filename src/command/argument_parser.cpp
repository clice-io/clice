#include "command/argument_parser.h"

#include <array>

#include "llvm/ADT/SmallString.h"
#include "llvm/Support/FileSystem.h"
#include "clang/Driver/Driver.h"
#include "clang/Driver/Options.h"

namespace clice {

namespace {

namespace opt = llvm::opt;
namespace driver = clang::driver;

/// Access private members of OptTable via the Thief pattern.
bool enable_dash_dash_parsing(const opt::OptTable& table);
bool enable_grouped_short_options(const opt::OptTable& table);

template <auto MP1, auto MP2>
struct Thief {
    friend bool enable_dash_dash_parsing(const opt::OptTable& table) {
        return table.*MP1;
    }

    friend bool enable_grouped_short_options(const opt::OptTable& table) {
        return table.*MP2;
    }
};

template struct Thief<&opt::OptTable::DashDashParsing, &opt::OptTable::GroupedShortOptions>;

auto& option_table = driver::getDriverOptTable();

}  // namespace

std::unique_ptr<llvm::opt::Arg> ArgumentParser::parse_one(unsigned& index) {
    assert(!enable_dash_dash_parsing(option_table));
    assert(!enable_grouped_short_options(option_table));
    return option_table.ParseOneArg(*this, index);
}

using ID = clang::driver::options::ID;

bool is_discarded_option(unsigned id) {
    switch(id) {
        /// Input file and output — we manage these ourselves.
        case ID::OPT_INPUT:
        case ID::OPT_c:
        case ID::OPT_o:
        case ID::OPT_dxc_Fc:
        case ID::OPT_dxc_Fo:

        /// PCH building.
        case ID::OPT_emit_pch:
        case ID::OPT_include_pch:
        case ID::OPT__SLASH_Yu:
        case ID::OPT__SLASH_Fp:

        /// Dependency scan.
        case ID::OPT_E:
        case ID::OPT_M:
        case ID::OPT_MM:
        case ID::OPT_MD:
        case ID::OPT_MMD:
        case ID::OPT_MF:
        case ID::OPT_MT:
        case ID::OPT_MQ:
        case ID::OPT_MG:
        case ID::OPT_MP:
        case ID::OPT_show_inst:
        case ID::OPT_show_encoding:
        case ID::OPT_show_includes:
        case ID::OPT__SLASH_showFilenames:
        case ID::OPT__SLASH_showFilenames_:
        case ID::OPT__SLASH_showIncludes:
        case ID::OPT__SLASH_showIncludes_user:

        /// C++ modules — we handle these ourselves.
        case ID::OPT_fmodule_file:
        case ID::OPT_fmodule_output:
        case ID::OPT_fprebuilt_module_path: return true;

        default: return false;
    }
}

bool is_user_content_option(unsigned id) {
    switch(id) {
        case ID::OPT_I:
        case ID::OPT_isystem:
        case ID::OPT_iquote:
        case ID::OPT_idirafter:
        case ID::OPT_D:
        case ID::OPT_U:
        case ID::OPT_include: return true;
        default: return false;
    }
}

bool is_include_path_option(unsigned id) {
    switch(id) {
        case ID::OPT_I:
        case ID::OPT_isystem:
        case ID::OPT_iquote:
        case ID::OPT_idirafter: return true;
        default: return false;
    }
}

bool is_xclang_option(unsigned id) {
    return id == ID::OPT_Xclang;
}

std::optional<std::uint32_t> get_option_id(llvm::StringRef argument) {
    llvm::SmallString<64> buffer = argument;

    if(argument.ends_with("=")) {
        buffer += "placeholder";
    }

    unsigned index = 1;
    std::array arguments = {"clang++", buffer.c_str(), "placeholder"};
    llvm::opt::InputArgList arg_list(arguments.data(), arguments.data() + arguments.size());

    if(auto arg = option_table.ParseOneArg(arg_list, index)) {
        return arg->getOption().getID();
    } else {
        return {};
    }
}

llvm::StringRef resource_dir() {
    static std::string dir = [] {
        // Use address of this lambda to locate our binary via dladdr/proc.
        static int anchor;
        auto exe = llvm::sys::fs::getMainExecutable("", &anchor);
        if(exe.empty()) {
            return std::string{};
        }
        return clang::driver::Driver::GetResourcesPath(exe);
    }();
    return dir;
}

bool is_codegen_option(unsigned id, const llvm::opt::Option& opt) {
    /// Debug info options form a group (-g, -gdwarf-*, -gsplit-dwarf, etc.).
    if(opt.matches(ID::OPT_DebugInfo_Group)) {
        return true;
    }

    switch(id) {
        /// Position-independent code — pure codegen, no macro or semantic effect.
        case ID::OPT_fPIC:
        case ID::OPT_fno_PIC:
        case ID::OPT_fpic:
        case ID::OPT_fno_pic:
        case ID::OPT_fPIE:
        case ID::OPT_fno_PIE:
        case ID::OPT_fpie:
        case ID::OPT_fno_pie:

        /// Frame pointer and unwind tables — pure codegen.
        case ID::OPT_fomit_frame_pointer:
        case ID::OPT_fno_omit_frame_pointer:
        case ID::OPT_funwind_tables:
        case ID::OPT_fno_unwind_tables:
        case ID::OPT_fasynchronous_unwind_tables:
        case ID::OPT_fno_asynchronous_unwind_tables:

        /// Stack protection — pure codegen.
        case ID::OPT_fstack_protector:
        case ID::OPT_fstack_protector_strong:
        case ID::OPT_fstack_protector_all:
        case ID::OPT_fno_stack_protector:

        /// Section splitting, LTO, semantic interposition — pure codegen/linker.
        case ID::OPT_fdata_sections:
        case ID::OPT_fno_data_sections:
        case ID::OPT_ffunction_sections:
        case ID::OPT_fno_function_sections:
        case ID::OPT_flto:
        case ID::OPT_flto_EQ:
        case ID::OPT_fno_lto:
        case ID::OPT_fsemantic_interposition:
        case ID::OPT_fno_semantic_interposition:
        case ID::OPT_fvisibility_inlines_hidden:

        /// Diagnostics output formatting — doesn't affect analysis.
        case ID::OPT_fcolor_diagnostics:
        case ID::OPT_fno_color_diagnostics:

        /// Floating-point codegen — doesn't define macros (unlike -ffast-math).
        case ID::OPT_ftrapping_math:
        case ID::OPT_fno_trapping_math: return true;

        default: return false;
    }
}

}  // namespace clice

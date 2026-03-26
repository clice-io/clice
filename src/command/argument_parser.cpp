#include "command/argument_parser.h"

#include "clang/Driver/Driver.h"

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

template struct Thief<&opt::OptTable::DashDashParsing,
                      &opt::OptTable::GroupedShortOptions>;

auto& option_table = driver::getDriverOptTable();

}  // namespace

std::unique_ptr<llvm::opt::Arg> ArgumentParser::parse_one(unsigned& index) {
    assert(!enable_dash_dash_parsing(option_table));
    assert(!enable_grouped_short_options(option_table));
    return option_table.ParseOneArg(*this, index);
}

using ID = clang::driver::options::ID;

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
        case ID::OPT_fno_trapping_math:
            return true;

        default:
            return false;
    }
}

}  // namespace clice

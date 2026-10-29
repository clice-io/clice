module clice:command.argument;

import :support.prelude;

namespace clice {

namespace option {

enum ID : unsigned {
    OPT_INVALID = 0,

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
    OPT_##ID,
#include "clang/Options/Options.inc"
#undef OPTION
};

enum DriverClass : unsigned {
    DefaultVis = 1u << 0,
    CLOption = 1u << 1,
    CC1Option = 1u << 2,
    CC1AsOption = 1u << 3,
    FlangOption = 1u << 4,
    FC1Option = 1u << 5,
    DXCOption = 1u << 6,
};

const kota::option::OptTable& table();

}  // namespace option

enum class CompilerFamily : std::uint8_t {
    Unknown,
    GCC,      // Covers gcc, g++, cc, c++, and versioned/arch variants
    Clang,    // Covers clang, clang++, and versioned variants (excluding clang-cl)
    MSVC,     // Covers cl
    ClangCL,  // Covers clang-cl explicitly, and clang --driver-mode=cl
    NVCC,     // Covers nvcc
    Intel,    // Covers icc, icpc, icx, dpcpp
    Zig,      // Covers zig cc / zig c++ (assumed GCC/Clang compatible for query)
};

CompilerFamily driver_family(llvm::StringRef driver);

/// The target a driver's name sets, as clang reads the name of its own
/// program (`riscv32-wch-elf-gcc.exe` compiles for riscv32-wch-elf); empty
/// when the name has no prefix naming an architecture.
std::string name_target(llvm::StringRef driver);

/// cl and clang-cl, which spell options and split command lines their own
/// way.
constexpr bool cl_mode(CompilerFamily family) {
    return family == CompilerFamily::MSVC || family == CompilerFamily::ClangCL;
}

/// The option-table visibility mask of a driver family: CL families see
/// /U-, /D-style options; the rest exclude them so Unix absolute paths
/// are not misparsed. None sees flang's.
unsigned family_visibility(CompilerFamily family);

/// How an argument participates in compilation semantics. Every parsed
/// argument is kept in the structured command with a class; renders select
/// by class instead of re-parsing.
enum class ArgClass : std::uint8_t {
    /// Affects frontend semantics (language, target, macros defined by mode).
    Semantic,

    /// Per-file user content: -I, -D, -U, -include, -isystem, -iquote,
    /// -idirafter. Excluded from toolchain queries and re-attached after.
    UserContent,

    /// Diagnostics presentation (-W*, -R*, -pedantic*, -w): affects which
    /// warnings are emitted but never the token stream.
    Diagnostics,

    /// Pure backend/linker concerns (-g, -ffunction-sections, ...): kept for the
    /// Full view, skipped by compile renders and command hashes.
    Codegen,

    /// Irrelevant to an LSP (outputs, PCH building, dependency scan, module
    /// flags we manage ourselves): kept for the Full view only.
    Discarded,

    /// Unrecognized token, kept verbatim (identity is the spelling). In the
    /// command hash — dropping an option we don't understand could merge
    /// commands that must differ — but never in compile renders; forwarded
    /// to toolchain queries only for NVCC query tokens.
    Unknown,

    /// The input slot: where the source file sat in the original command.
    /// A pure position marker — it stores no path (the file is the entry's,
    /// not the command's). Language selectors before it govern the input;
    /// renders inject the actual file here.
    Input,
};

struct Arg {
    std::uint32_t opt_id = 0;
    ArgClass cls = ArgClass::Semantic;

    /// The alias of the option the command wrote (`-msmall-data-limit=` for
    /// -G), 0 when it wrote the option's own name.
    std::uint16_t alias = 0;

    /// Interned verbatim token, only for ArgClass::Unknown (nullptr otherwise).
    const char* spelling = nullptr;

    /// Interned option values (paths already absolutized for include-path
    /// options). Elements are pointer-comparable.
    llvm::ArrayRef<const char*> values;

    friend bool operator==(const Arg& lhs, const Arg& rhs) {
        return lhs.opt_id == rhs.opt_id && lhs.cls == rhs.cls && lhs.alias == rhs.alias &&
               lhs.spelling == rhs.spelling && lhs.values == rhs.values;
    }
};

/// An argument on its way into a command: its values are interned, their
/// array stays local until the command is saved.
struct LocalArg {
    std::uint32_t opt_id = 0;
    ArgClass cls = ArgClass::Semantic;
    std::uint16_t alias = 0;
    const char* spelling = nullptr;
    llvm::SmallVector<const char*, 2> values;

    Arg view() const {
        return {.opt_id = opt_id,
                .cls = cls,
                .alias = alias,
                .spelling = spelling,
                .values = values};
    }
};

/// Check if an option is a codegen-only flag that doesn't affect frontend
/// semantics (parsing, diagnostics, code completion). These are pure
/// backend/linker concerns irrelevant to an LSP server.
///
/// Note: options that DO affect semantics are intentionally kept:
///   -fno-exceptions, -fno-rtti, -std=*, -march=*, -fsanitize=*, -O*, -W*,
///   and -fPIC and -fstack-protector*, which define __PIC__ and __SSP__
bool is_codegen_option(unsigned id);

/// Options that are completely irrelevant to an LSP and should be discarded
/// (input/output, PCH building, dependency scan, C++ modules, and anything
/// the driver itself accepts and ignores).
bool is_discarded_option(unsigned id);

/// User-content options that go into the per-file patch rather than the
/// shared canonical command: -I, -D, -U, -include, -imacros, -isystem, -iquote,
/// -idirafter.
bool is_user_content_option(unsigned id);

ArgClass classify_option(unsigned id, llvm::ArrayRef<const char*> values);

/// Whether `value` of option `id` names a file or directory, which every
/// reader of the command spells the way clice spells paths (Spelling): a
/// relative one anchored at the directory the command runs in. A leading
/// `=` or `$SYSROOT` is the sysroot, which clang substitutes; a bare
/// `--config` name is searched in clang's configuration directories.
bool names_path(unsigned id, llvm::StringRef value);

/// Check if this is the -Xclang pass-through option.
bool is_xclang_option(unsigned id);

/// Diagnostics-presentation options (-W*, -R*, -pedantic*, -w): they affect
/// which warnings are emitted but never the token stream produced by
/// preprocessing.
bool is_diagnostics_option(unsigned id);

/// Render one structured argument back into argv fragments. Unknown args
/// render as their verbatim spelling; known options render through the
/// option table (canonical name + render style). Never called on the
/// input slot.
void render_arg(const Arg& arg, llvm::function_ref<void(std::string_view)> cb);

/// The alias of `parsed`'s option that the command wrote, 0 when it wrote
/// the option's own name: the parse resolves an alias to its option.
std::uint16_t written_alias(const kota::option::ParsedArg& parsed);

/// Whether the command's own driver gets `arg` through the alias the
/// command wrote: a GCC-style driver, which may know that name alone, when
/// the alias is one of its own (a rule taking `--driver-mode=cl` out of a
/// command leaves the cl spellings its parse read).
bool keeps_alias(const Arg& arg, CompilerFamily family);

/// Render one argument for the command's own driver. A GCC-style driver
/// gets an option as the command wrote it, which another compiler may know
/// by that name alone: GCC's `-msmall-data-limit=8` is clang's `-G 8`,
/// which RISC-V GCC rejects. A cl-mode driver reads a `-` spelling as its
/// own option where one exists (`-Wall` is `/Wall`, -Weverything) and
/// drops the GCC-style ones it lacks, so an argument the parse unaliased
/// out of a cl spelling (`/W3` is -Wall, `/J` -funsigned-char) is spelled
/// through a cl alias of its option, else through `/clang:`.
void render_driver_arg(const Arg& arg,
                       CompilerFamily family,
                       llvm::function_ref<void(std::string_view)> cb);

/// Render the frontend-semantic options of `args` — preprocessing,
/// language and warnings, without codegen-only options and those clice
/// manages itself — into a stable string suitable for hashing (fragments
/// are NUL-separated).  This is a pure filter: the relative order of kept
/// options is preserved because it is semantically significant (-I/-D
/// ordering).  args[0] must be the driver; it is always kept since it
/// affects language defaults.  Arguments that fail to parse are kept
/// verbatim — over-keying is safe, under-keying is not.
std::string canonicalize(llvm::ArrayRef<std::string> args);

}  // namespace clice

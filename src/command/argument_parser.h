#pragma once

#include <cstdint>
#include <string>

#include <kota/deco/option.h>
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringRef.h"

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
               ...)                                                                                \
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

/// Check if an option is a codegen-only flag that doesn't affect frontend
/// semantics (parsing, diagnostics, code completion). These are pure
/// backend/linker concerns irrelevant to an LSP server.
///
/// Note: options that DO affect semantics are intentionally kept:
///   -fno-exceptions, -fno-rtti, -std=*, -march=*, -fsanitize=*, -O*, -W*
///
/// Defined out-of-line in argument_parser.cpp (needs clang driver option IDs).
bool is_codegen_option(unsigned id);

/// Options that are completely irrelevant to an LSP and should be discarded
/// (input/output, PCH building, dependency scan, C++ modules).
bool is_discarded_option(unsigned id);

/// User-content options that go into the per-file patch rather than the
/// shared canonical command: -I, -D, -U, -include, -isystem, -iquote, -idirafter.
bool is_user_content_option(unsigned id);

/// Subset of user-content options that are include-path flags
/// (-I, -isystem, -iquote, -idirafter) — used for path absolutization.
bool is_include_path_option(unsigned id);

/// Check if this is the -Xclang pass-through option.
bool is_xclang_option(unsigned id);

/// Diagnostics-presentation options (-W*, -R*, -pedantic*, -w): they affect
/// which warnings are emitted but never the token stream produced by
/// preprocessing.
bool is_diagnostics_option(unsigned id);

/// Which subset of a compile command matters when deriving a cache key.
enum class ArgsProfile : std::uint8_t {
    /// Options that can influence preprocessing: include paths, macros,
    /// language dialect and target (predefined macros).  Computed
    /// subtractively as Frontend minus diagnostics-presentation options,
    /// because nearly every language option can define a feature macro —
    /// over-inclusion only costs sharing, under-inclusion costs correctness.
    Preprocessing,

    /// All frontend-semantic options: preprocessing + language + warnings.
    /// Excludes codegen-only options and options clice manages itself.
    Frontend,

    /// Every recognized option, including codegen and inputs.
    Full,
};

/// Render the subset of `args` selected by `profile` into a stable string
/// suitable for hashing (fragments are NUL-separated).  This is a pure
/// filter: the relative order of kept options is preserved because it is
/// semantically significant (-I/-D ordering).  args[0] must be the driver;
/// it is always kept since it affects language defaults.  Arguments that
/// fail to parse are kept verbatim — over-keying is safe, under-keying is
/// not.
std::string canonicalize(llvm::ArrayRef<std::string> args, ArgsProfile profile);

/// Get the resource directory for clang builtin headers. Computed once
/// from the current executable path using Driver::GetResourcesPath.
llvm::StringRef resource_dir();

/// Format an argument list as a human-readable string: "[arg1 arg2 ...]".
std::string print_argv(llvm::ArrayRef<const char*> args);

/// Return the visibility mask to exclude MSVC cl.exe-style options (/U, /D,
/// /I, etc.) unless the driver is cl.exe.  This prevents Unix absolute paths
/// like /Users/... from being misparsed as /U sers/... on macOS/Linux.
/// Defined out-of-line in argument_parser.cpp (needs ClangVisibility enum).
unsigned default_visibility(llvm::StringRef driver);

/// Check if a filename has a C/C++/ObjC/CUDA/etc. extension accepted by clang.
/// Returns false for .rc (Windows resource), .asm, .def, and other non-C-family files.
/// Defined out-of-line in argument_parser.cpp (needs clang::driver::types).
bool is_c_family_file(llvm::StringRef filename);

}  // namespace clice

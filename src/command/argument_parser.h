#pragma once

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
               VALUES)                                                                             \
    OPT_##ID,
#include "clang/Driver/Options.inc"
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

/// Options that affect system path discovery and should be included in the
/// toolchain cache key. Only these flags are passed to the toolchain query.
bool is_toolchain_option(unsigned id);

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

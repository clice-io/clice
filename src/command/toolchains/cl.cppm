module;

#include "support/prelude.h"

module clice:command.toolchains.cl;

import :command.argument;
import :command.toolchains.clang;

namespace clice {

/// What cl or clang-cl tells when asked to preprocess (`/EP`)
/// `cl_query_source`: the -std value clang knows for the C++ standard it
/// compiles in, which `_MSVC_LANG` names whatever `/Zc:__cplusplus` says,
/// and its version as `-fms-compatibility-version` takes it
/// (`_MSC_FULL_VER` 194435207 is 19.44.35207). Each is empty when its
/// macro is not defined: a C input has no `_MSVC_LANG`.
struct CLOutput {
    std::string standard;

    std::string version;
};

constexpr inline llvm::StringRef cl_query_source =
    "msvc_lang _MSVC_LANG\nmsc_full_ver _MSC_FULL_VER\n";

/// Read the output cl wrote to stdout for `cl_query_source`.
CLOutput parse_cl_output(llvm::StringRef out);

/// What clang's own driver is told for a command of cl or clang-cl beyond
/// its flags: the cl mode, and for a cl.exe in a toolset,
/// `<toolset>/bin/Host<arch>/<arch>/cl.exe`, that toolset. Without one,
/// clang takes the headers of the newest toolset installed, outside a
/// developer prompt (an editor started clice); in one, the `INCLUDE` it
/// sets, which naming a toolset would make clang ignore, lists the build's.
std::vector<std::string> cl_driver_mode(llvm::StringRef driver, CompilerFamily family);

/// cl and clang-cl: asked to preprocess a file naming their macros, the
/// driver tells the C++ standard it compiles in — what `/std:c++latest`
/// means moves from one version to the next — and cl its version, where
/// clang's own detection would take the newest toolset installed. clang's
/// own driver in cl mode, which sets the windows-msvc target itself, then
/// builds the job in that standard and version. clang-cl's version is the
/// one that detection finds, from the toolset its flags select
/// (`/winsysroot`), as it does here.
kota::task<std::expected<Job, std::string>> ask_cl(llvm::ArrayRef<std::string> args,
                                                   CompilerFamily family,
                                                   llvm::StringRef input,
                                                   std::string cwd);

}  // namespace clice

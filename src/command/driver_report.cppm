module;

#include "modules/prelude.h"

module clice:command.driver_report;

namespace clice {

/// One directory of a GCC driver's header search list.
struct ReportedDir {
    std::string path;

    /// Listed as a macOS framework directory.
    bool framework = false;
};

/// What a GCC-style driver reports when asked to preprocess an empty file
/// verbosely, printing its macros (`driver <flags> -E -dM -v`): the job it
/// runs and the language standard it compiles in. A driver that is clang
/// underneath (Apple clang, zig, emcc, icx, a `c++` that is clang) shows a
/// clang job; GCC shows its own cc1 job, its target and its header search
/// list.
struct DriverReport {
    /// The clang job: its program, -cc1 and its arguments as printed;
    /// empty for GCC.
    std::vector<std::string> clang_job;

    /// Whether GCC ran its compiler proper; the fields up to `private_dir`
    /// are GCC's.
    bool gcc_job = false;

    /// The target triple, from the `Target:` line.
    std::string target;

    /// The options GCC's driver passed its cc1 job that reach clang's
    /// driver too: macros (`-D_REENTRANT` from the specs), target defaults
    /// (`-march=armv7-a+fp -mthumb`), hardening (`-fstack-protector-strong`).
    std::vector<std::string> gcc_options;

    /// GCC's `#include <...>` search list, in order.
    std::vector<ReportedDir> search_dirs;

    /// The entry of `search_dirs` that is GCC's own private include
    /// directory (lib/gcc/<target>/<version>/include), whose place clang's
    /// builtin headers take; none under -nostdinc.
    std::optional<std::size_t> private_dir;

    /// The language standard the driver compiles in, as a -std value clang
    /// knows.
    std::string standard;
};

/// Read the report from the program's stdout (`out`) and stderr (`err`).
DriverReport parse_driver_report(llvm::StringRef out, llvm::StringRef err);

/// What cl or clang-cl tells when asked to preprocess (`/EP`)
/// `cl_probe_source`: the -std value clang knows for the C++ standard it
/// compiles in, which `_MSVC_LANG` names whatever `/Zc:__cplusplus` says,
/// and its version as `-fms-compatibility-version` takes it
/// (`_MSC_FULL_VER` 194435207 is 19.44.35207). Each is empty when its
/// macro is not defined: a C input has no `_MSVC_LANG`.
struct CLReport {
    std::string standard;

    std::string version;
};

constexpr inline llvm::StringRef cl_probe_source =
    "msvc_lang _MSVC_LANG\nmsc_full_ver _MSC_FULL_VER\n";

/// Read the report from what cl wrote to stdout for `cl_probe_source`.
CLReport parse_cl_report(llvm::StringRef out);

/// The first clang job a driver prints under `-v`: its program, -cc1 and
/// its arguments as printed. Empty when there is none.
std::vector<std::string> parse_clang_job(llvm::StringRef output);

}  // namespace clice

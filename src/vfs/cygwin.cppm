module;

#include "support/prelude.h"

module clice:vfs.cygwin;

namespace clice {

/// The mount table, read from the runtime's own `mount`, of the Cygwin or
/// MSYS2 runtime that spelled `written`, POSIX paths of files that exist:
/// of the runtimes (cygwin1.dll, msys-2.0.dll) beside `program` and on PATH,
/// in the order Windows looks for a program's DLLs, the first whose table
/// maps each of them to a file that exists. Taking the first runtime alone
/// would be wrong where PATH holds two, Git for Windows' MSYS2 ahead of the
/// Cygwin a toolchain outside it loads. An empty `program` searches PATH
/// alone.
kota::task<std::expected<fs::cygwin::MountTable, std::string>>
    find_mount_table(std::string program, std::vector<std::string> written);

}  // namespace clice

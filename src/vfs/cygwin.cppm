module;

#include "support/prelude.h"

module clice:vfs.cygwin;

namespace clice {

/// Whether `path` is a POSIX path a Cygwin or MSYS2 program wrote
/// (`/usr/include`, `/cygdrive/c/proj`), which a native program cannot
/// open: rooted, but not absolute here. Never outside Windows, where every
/// rooted path is native.
bool is_posix_path(llvm::StringRef path);

/// The mount table of a Cygwin or MSYS2 runtime, which maps the POSIX
/// paths its programs print and take to Windows paths. A mapping assumed
/// instead of read from the runtime gets them wrong: Cygwin mounts
/// `/usr/lib` at `<root>/lib`, MSYS2 its drives at `/c` rather than
/// `/cygdrive/c`, and a user's /etc/fstab adds mounts of its own.
struct MountTable {
    /// Each mount point with the Windows directory it names, the longest
    /// point first.
    std::vector<std::pair<std::string, std::string>> mounts;

    /// The table as the runtime's `mount` lists it: one
    /// `<windows> on <point> type <fs> (<flags>)` line per mount.
    static MountTable parse(llvm::StringRef listing);

    /// The Windows path `posix` names, with forward slashes. `.` and `..`
    /// fold first, as the runtime folds them: `..` out of a mount point
    /// leaves the mount.
    std::string to_windows(llvm::StringRef posix) const;
};

/// The mount table, read from the runtime's own `mount`, of the Cygwin or
/// MSYS2 runtime that spelled `written`, POSIX paths of files that exist:
/// of the runtimes (cygwin1.dll, msys-2.0.dll) beside `program` and on PATH,
/// in the order Windows looks for a program's DLLs, the first whose table
/// maps each of them to a file that exists. Taking the first runtime alone
/// would be wrong where PATH holds two, Git for Windows' MSYS2 ahead of the
/// Cygwin a toolchain outside it loads. An empty `program` searches PATH
/// alone.
kota::task<std::expected<MountTable, std::string>>
    find_mount_table(std::string program, std::vector<std::string> written);

/// The target of the Cygwin symlink at `path`, a file holding the cookie
/// `!<symlink>` and the target, which the package installer writes for a
/// package's links (`/usr/bin/cc`) and Windows cannot follow. Nullopt when
/// `path` is no such file.
std::optional<std::string> read_cygwin_symlink(llvm::StringRef path);

}  // namespace clice

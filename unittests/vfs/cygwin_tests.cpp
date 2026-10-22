module;

#include "modules/prelude.h"

module clice;

import :unittests.utils.temp_dir;
import :unittests.utils;
import :vfs.cygwin;

namespace clice::testing {
namespace {

ZEST_SUITE(CygwinTests) {

/// Cygwin 3.6's `mount`.
constexpr static llvm::StringRef cygwin_mounts =
    R"(C:/cygwin64/bin on /usr/bin type ntfs (binary,auto)
C:/cygwin64/lib on /usr/lib type ntfs (binary,auto)
C:/cygwin64 on / type ntfs (binary,auto)
C: on /cygdrive/c type ntfs (binary,posix=0,user,noumount,auto)
D: on /cygdrive/d type ntfs (binary,posix=0,user,noumount,auto)
)";

ZEST_CASE(CygwinMounts) {
    auto table = MountTable::parse(cygwin_mounts);
    ZEXPECT(table.mounts.size() == std::size_t(5));
    ZEXPECT(table.to_windows("/usr/include") == "C:/cygwin64/usr/include");
    ZEXPECT(table.to_windows("/usr/lib/gcc/x86_64-pc-cygwin/14/include") ==
            "C:/cygwin64/lib/gcc/x86_64-pc-cygwin/14/include");
    ZEXPECT(table.to_windows("/usr/libexec") == "C:/cygwin64/usr/libexec");
    ZEXPECT(table.to_windows("/") == "C:/cygwin64");
    ZEXPECT(table.to_windows("/cygdrive/c/work/proj ü x/src/main.cpp") ==
            "C:/work/proj ü x/src/main.cpp");
    ZEXPECT(table.to_windows("/cygdrive/d") == "D:/");
    /// The runtime takes a drive's letter in either case.
    ZEXPECT(table.to_windows("/cygdrive/D/x") == "D:/x");
}

ZEST_CASE(DotsFoldFirst) {
    /// GCC's w32api directory leaves the /usr/lib mount: folded where the
    /// runtime folds it, it is <root>/usr/include/w32api, not C:/include.
    auto table = MountTable::parse(cygwin_mounts);
    ZEXPECT(table.to_windows("/usr/lib/gcc/x86_64-pc-cygwin/14/../../../../include/w32api") ==
            "C:/cygwin64/usr/include/w32api");
    ZEXPECT(table.to_windows("/cygdrive/c/Andes/bin/../lib/gcc/riscv32-elf/12.2.0/include") ==
            "C:/Andes/lib/gcc/riscv32-elf/12.2.0/include");
}

ZEST_CASE(MSYS2Mounts) {
    /// MSYS2 mounts its drives at the root, and /bin at <root>/usr/bin.
    auto table = MountTable::parse(R"(C:/msys64 on / type ntfs (binary,noacl,auto)
C:/msys64/usr/bin on /bin type ntfs (binary,noacl,auto)
C: on /c type ntfs (binary,noacl,posix=0,user,noumount,auto)
)");
    ZEXPECT(table.to_windows("/c/proj/a.c") == "C:/proj/a.c");
    ZEXPECT(table.to_windows("/C/proj/a.c") == "C:/proj/a.c");
    ZEXPECT(table.to_windows("/bin/gcc.exe") == "C:/msys64/usr/bin/gcc.exe");
    ZEXPECT(table.to_windows("/usr/include") == "C:/msys64/usr/include");
    /// A mount point covers its own directory only, not a longer name.
    ZEXPECT(table.to_windows("/binary") == "C:/msys64/binary");
}

ZEST_CASE(SpacesInMounts) {
    auto table = MountTable::parse(
        R"(C:/Program Files/Andes Tech/cygwin on / type ntfs (binary,auto)
C:/Program Files/Andes Tech/cygwin/bin on /usr/bin type ntfs (binary,auto)
)");
    ZEXPECT(table.to_windows("/usr/bin/cygpath.exe") ==
            "C:/Program Files/Andes Tech/cygwin/bin/cygpath.exe");
}

ZEST_CASE(PosixPaths) {
    ZEXPECT(!is_posix_path("relative/a.h"));
    ZEXPECT(!is_posix_path(TempDir().path("a.h")));
    ZEXPECT(is_posix_path("/usr/include") == Windows);
}

ZEST_CASE(SymlinkFiles) {
    TempDir dir;
    /// The installer's form, as Cygwin's gcc-core package installs
    /// /usr/bin/cc, and the runtime's own, in UTF-16.
    dir.touch("cc", llvm::StringRef("!<symlink>gcc.exe\0", 18));
    ZEXPECT(read_cygwin_symlink(dir.path("cc")) == std::optional<std::string>("gcc.exe"));
    dir.touch("c++", llvm::StringRef("!<symlink>\xFF\xFEg\0+\0+\0\0\0", 20));
    ZEXPECT(read_cygwin_symlink(dir.path("c++")) == std::optional<std::string>("g++"));
    dir.touch("gcc.exe", "MZ");
    ZEXPECT(!read_cygwin_symlink(dir.path("gcc.exe")));
    ZEXPECT(!read_cygwin_symlink(dir.path("missing")));
}

};  // ZEST_SUITE(CygwinTests)

}  // namespace
}  // namespace clice::testing

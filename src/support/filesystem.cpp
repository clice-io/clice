#include "support/filesystem.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "llvm/ADT/ScopeExit.h"
#include "llvm/Support/WindowsError.h"

namespace llvm::sys::windows {

// Declared by llvm/Support/Windows/WindowsSupport.h, which is not
// installed.
std::error_code widenPath(const Twine& path8,
                          SmallVectorImpl<wchar_t>& path16,
                          size_t max_path_len = MAX_PATH);

}  // namespace llvm::sys::windows
#endif

namespace clice::fs {

std::error_code remove(const llvm::Twine& path, bool ignore_non_existing) {
#ifdef _WIN32
    llvm::SmallVector<wchar_t, 256> wide;
    if(auto error = llvm::sys::windows::widenPath(path, wide)) {
        return error;
    }
    wide.push_back(L'\0');
    HANDLE handle = ::CreateFileW(wide.data(),
                                  DELETE,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                  nullptr,
                                  OPEN_EXISTING,
                                  FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                                  nullptr);
    if(handle == INVALID_HANDLE_VALUE) {
        auto error = llvm::mapWindowsError(::GetLastError());
        if(ignore_non_existing && error == std::errc::no_such_file_or_directory) {
            return {};
        }
        return error;
    }
    auto close = llvm::make_scope_exit([&] { ::CloseHandle(handle); });
    FILE_DISPOSITION_INFO_EX posix{FILE_DISPOSITION_FLAG_DELETE |
                                   FILE_DISPOSITION_FLAG_POSIX_SEMANTICS};
    if(::SetFileInformationByHandle(handle, FileDispositionInfoEx, &posix, sizeof(posix))) {
        return {};
    }
    // Filesystems without POSIX deletes (FAT) refuse the flag: a mapped
    // file cannot go there at all.
    FILE_DISPOSITION_INFO classic{TRUE};
    if(::SetFileInformationByHandle(handle, FileDispositionInfo, &classic, sizeof(classic))) {
        return {};
    }
    return llvm::mapWindowsError(::GetLastError());
#else
    return llvm::sys::fs::remove(path, ignore_non_existing);
#endif
}

}  // namespace clice::fs

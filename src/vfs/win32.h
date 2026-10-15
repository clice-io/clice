#pragma once

#ifdef _WIN32

// Included after modules/prelude.h, which imports std and LLVM: their
// headers here would be parsed again beside the modules.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace llvm::sys::windows {

// Declared by llvm/Support/Windows/WindowsSupport.h, which pins
// _WIN32_WINNT below the version that declares GetFileInformationByName.
std::error_code widenPath(const Twine& path8,
                          SmallVectorImpl<wchar_t>& path16,
                          size_t max_path_len = MAX_PATH);

}  // namespace llvm::sys::windows

namespace clice::vfs {

/// `path` as the Windows file functions take it: UTF-16, null-terminated,
/// and prefixed `\\?\` once it is too long for them without.
inline std::error_code widen(llvm::StringRef path, llvm::SmallVectorImpl<wchar_t>& wide) {
    if(auto error = llvm::sys::windows::widenPath(path, wide)) {
        return error;
    }
    wide.push_back(L'\0');
    return {};
}

/// A handle to the file or directory `path` names, opened for `access`
/// with every sharing mode, so it never refuses other processes; `flags`
/// add to FILE_FLAG_BACKUP_SEMANTICS, which opening a directory needs.
/// The caller closes it.
inline std::expected<HANDLE, std::error_code> open_handle(llvm::StringRef path,
                                                          DWORD access,
                                                          DWORD flags = 0) {
    llvm::SmallVector<wchar_t, 256> wide;
    if(auto error = widen(path, wide)) {
        return std::unexpected(error);
    }
    HANDLE handle = ::CreateFileW(wide.data(),
                                  access,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                  nullptr,
                                  OPEN_EXISTING,
                                  FILE_FLAG_BACKUP_SEMANTICS | flags,
                                  nullptr);
    if(handle == INVALID_HANDLE_VALUE) {
        return std::unexpected(llvm::mapWindowsError(::GetLastError()));
    }
    return handle;
}

/// Whether an entry with these attributes and reparse tag is a link — a
/// symlink or a junction. Other reparse points (cloud placeholders,
/// deduplicated or compressed files) are the file or directory they hold.
inline bool is_link(DWORD attributes, DWORD tag) {
    return (attributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
           (tag == IO_REPARSE_TAG_SYMLINK || tag == IO_REPARSE_TAG_MOUNT_POINT);
}

}  // namespace clice::vfs

#endif

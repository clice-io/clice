#pragma once

#ifdef _WIN32

#include <system_error>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/Twine.h"

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

/// Whether an entry with these attributes and reparse tag is a link — a
/// symlink or a junction. Other reparse points (cloud placeholders,
/// deduplicated or compressed files) are the file or directory they hold.
inline bool is_link(DWORD attributes, DWORD tag) {
    return (attributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
           (tag == IO_REPARSE_TAG_SYMLINK || tag == IO_REPARSE_TAG_MOUNT_POINT);
}

}  // namespace clice::vfs

#endif

#include "support/filesystem.h"

#ifdef _WIN32
#include <windows.h>

#include "llvm/Support/ConvertUTF.h"
#include "llvm/Support/Windows/WindowsSupport.h"
#include "llvm/Support/WindowsError.h"
#endif

namespace clice {

std::error_code fs::file_metadata(const llvm::Twine& path, FileMetadata& result) {
#ifdef _WIN32
    llvm::SmallVector<wchar_t, 256> wide;
    if(auto ec = llvm::sys::windows::widenPath(path, wide)) {
        return ec;
    }
    WIN32_FILE_ATTRIBUTE_DATA data;
    if(::GetFileAttributesExW(wide.data(), GetFileExInfoStandard, &data) &&
       !(data.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DEVICE))) {
        result = {};
        result.size = (std::uint64_t(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
        result.mtime_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                              llvm::sys::toTimePoint(data.ftLastWriteTime).time_since_epoch())
                              .count();
        return {};
    }
    // Attribute queries describe a link itself. LLVM follows the target and
    // also preserves its handling of device paths and attribute-query errors.
#endif
    llvm::sys::fs::file_status status;
    if(auto ec = llvm::sys::fs::status(path, status)) {
        return ec;
    }
    result = FileMetadata(status);
    return {};
}

std::error_code fs::file_metadata(llvm::sys::fs::file_t file, FileMetadata& result) {
#ifdef _WIN32
    BY_HANDLE_FILE_INFORMATION info;
    if(!::GetFileInformationByHandle(file, &info)) {
        return llvm::mapWindowsError(::GetLastError());
    }
    result = {};
    result.size = (std::uint64_t(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
    result.mtime_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                          llvm::sys::toTimePoint(info.ftLastWriteTime).time_since_epoch())
                          .count();
    return {};
#else
    llvm::sys::fs::file_status status;
    if(auto ec = llvm::sys::fs::status(file, status)) {
        return ec;
    }
    result = FileMetadata(status);
    return {};
#endif
}

}  // namespace clice

#pragma once

#include <deque>
#include <string>

#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"

namespace clice::testing {

/// RAII helper for a temporary directory tree.
///
/// Creates a unique temporary directory on construction and removes it
/// (recursively) on destruction.  Provides helpers for building paths,
/// creating sub-directories, and writing files — used across multiple
/// test suites that need real filesystem state.
///
/// Also serves as a cross-platform source of absolute paths: on Windows
/// the root includes a drive letter, so `path("x")` is absolute everywhere.
struct TempDir {
    llvm::SmallString<128> root;

    TempDir(llvm::StringRef prefix = "clice-test") {
        llvm::sys::fs::createUniqueDirectory(prefix, root);
    }

    ~TempDir() {
        llvm::sys::fs::remove_directories(root);
    }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    /// Build an absolute path under this temporary root.
    std::string path(llvm::StringRef relative) const {
        llvm::SmallString<256> result(root);
        llvm::sys::path::append(result, relative);
        llvm::sys::path::native(result);
        return std::string(result);
    }

    /// Like path(), but returns a `const char*` whose lifetime is tied to
    /// this TempDir.  Useful for building `ArrayRef<const char*>` argument
    /// lists without manual lifetime management.
    const char* c_path(llvm::StringRef relative) {
        pool.push_back(path(relative));
        return pool.back().c_str();
    }

    /// Create a sub-directory (and any parents).
    void mkdir(llvm::StringRef relative) {
        llvm::sys::fs::create_directories(path(relative));
    }

    /// Create a file with optional content (parent dirs created automatically).
    void touch(llvm::StringRef relative, llvm::StringRef content = "") {
        auto p = path(relative);
        llvm::sys::fs::create_directories(llvm::sys::path::parent_path(p));
        std::error_code ec;
        llvm::raw_fd_ostream out(p, ec);
        if(!ec) {
            out << content;
        }
    }

private:
    /// Pool for strings returned by c_path(). std::deque guarantees that
    /// existing elements are not moved when new ones are appended.
    std::deque<std::string> pool;
};

}  // namespace clice::testing

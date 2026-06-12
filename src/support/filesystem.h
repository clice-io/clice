#pragma once

#include <cassert>
#include <cstdlib>
#include <expected>
#include <memory>
#include <print>
#include <string>

#include "support/format.h"

#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/VirtualFileSystem.h"

namespace clice {

namespace path {

using namespace llvm::sys::path;

template <typename... Args>
std::string join(Args&&... args) {
    llvm::SmallString<128> path;
    ((path::append(path, std::forward<Args>(args))), ...);
    return path.str().str();
}

inline std::string real_path(llvm::StringRef file) {
    llvm::SmallString<128> path;
    auto error = llvm::sys::fs::real_path(file, path);
    if(error) {
        std::println("Failed to get real path of {}: {}", file, error.message());
        std::abort();
    }
    return path.str().str();
}

inline std::string default_socket_path() {
    llvm::SmallString<128> home;
    if(!llvm::sys::path::home_directory(home))
        return "/tmp/clice.sock";
    llvm::sys::path::append(home, ".clice", "clice.sock");
    return home.str().str();
}

}  // namespace path

namespace fs {

using namespace llvm::sys::fs;

using llvm::sys::fs::createTemporaryFile;

inline std::expected<std::string, std::error_code> createTemporaryFile(llvm::StringRef prefix,
                                                                       llvm::StringRef suffix) {
    llvm::SmallString<128> path;
    auto error = llvm::sys::fs::createTemporaryFile(prefix, suffix, path);
    if(error) {
        return std::unexpected(error);
    }
    return path.str().str();
}

inline std::expected<void, std::error_code> write(llvm::StringRef path, llvm::StringRef content) {
    std::error_code EC;
    llvm::raw_fd_ostream os(path, EC, llvm::sys::fs::OF_None);
    if(EC) {
        return std::unexpected(EC);
    }
    os << content;
    os.flush();
    return std::expected<void, std::error_code>();
}

inline std::expected<std::string, std::error_code> read(llvm::StringRef path) {
    auto buffer = llvm::MemoryBuffer::getFile(path);
    if(!buffer) {
        return std::unexpected(buffer.getError());
    }
    return buffer.get()->getBuffer().str();
}

inline std::expected<void, std::error_code> rename(llvm::StringRef from, llvm::StringRef to) {
    auto error = llvm::sys::fs::rename(from, to);
    if(error) {
        return std::unexpected(error);
    }
    return std::expected<void, std::error_code>();
}

/// Recursively remove a directory tree using plain filesystem primitives.
/// Use this instead of llvm::sys::fs::remove_directories: on Windows that
/// is implemented over shell COM (CoInitializeEx + IFileOperation), which
/// initializes apartment COM on the calling thread and silently no-ops
/// when that fails — unsuitable for server threads.  This mirrors the
/// recursion LLVM's own Unix implementation uses.  Symlinks — including a
/// symlinked root — are removed without following them, so the recursion
/// can never escape into the link's target.  Returns the first error; a
/// missing path is not an error.
inline std::error_code remove_all(llvm::StringRef target) {
    llvm::sys::fs::file_status target_status;
    if(auto status_ec = llvm::sys::fs::status(target, target_status, /*follow=*/false)) {
        return status_ec == std::errc::no_such_file_or_directory ? std::error_code() : status_ec;
    }
    if(target_status.type() != llvm::sys::fs::file_type::directory_file) {
        return llvm::sys::fs::remove(target, /*IgnoreNonExisting=*/true);
    }

    std::error_code ec;
    for(llvm::sys::fs::directory_iterator it(target, ec, /*follow_symlinks=*/false), end;
        !ec && it != end;
        it.increment(ec)) {
        auto status = it->status();
        if(!status) {
            return status.getError();
        }
        if(llvm::sys::fs::is_directory(*status)) {
            if(auto sub_ec = remove_all(it->path())) {
                return sub_ec;
            }
        } else if(auto remove_ec = llvm::sys::fs::remove(it->path(), /*IgnoreNonExisting=*/true)) {
            return remove_ec;
        }
    }
    if(ec) {
        // A missing root is fine: there is simply nothing to remove.
        return ec == std::errc::no_such_file_or_directory ? std::error_code() : ec;
    }
    return llvm::sys::fs::remove(target, /*IgnoreNonExisting=*/true);
}

}  // namespace fs

namespace vfs = llvm::vfs;

class ThreadSafeFS : public vfs::ProxyFileSystem {
public:
    explicit ThreadSafeFS() : ProxyFileSystem(vfs::createPhysicalFileSystem()) {}

    class VolatileFile : public vfs::File {
    public:
        explicit VolatileFile(std::unique_ptr<vfs::File> wrapped) : wrapped(std::move(wrapped)) {
            assert(this->wrapped);
        }

        llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>> getBuffer(const llvm::Twine& Name,
                                                                     int64_t FileSize,
                                                                     bool RequiresNullTerminator,
                                                                     bool /*IsVolatile*/) override {
            return wrapped->getBuffer(Name,
                                      FileSize,
                                      RequiresNullTerminator,
                                      /*IsVolatile=*/true);
        }

        llvm::ErrorOr<vfs::Status> status() override {
            return wrapped->status();
        }

        llvm::ErrorOr<std::string> getName() override {
            return wrapped->getName();
        }

        std::error_code close() override {
            return wrapped->close();
        }

    private:
        std::unique_ptr<File> wrapped;
    };

    llvm::ErrorOr<std::unique_ptr<vfs::File>> openFileForRead(const llvm::Twine& InPath) override {
        llvm::SmallString<128> Path;
        InPath.toVector(Path);

        auto file = getUnderlyingFS().openFileForRead(Path);
        if(!file) {
            return file;
        }

        llvm::StringRef filename = path::filename(Path);
        if(filename.ends_with(".pch")) {
            return file;
        }
        return std::make_unique<VolatileFile>(std::move(*file));
    }
};

}  // namespace clice

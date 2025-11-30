#pragma once

#include <expected>

#include "Assert.h"

#include "llvm/ADT/StringExtras.h"
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

/// Get the real path of the given file. The file must exist. If the file does not exist,

inline std::string real_path(llvm::StringRef file) {
    llvm::SmallString<128> path;
    auto error = llvm::sys::fs::real_path(file, path);
    ASSERT(!error, "Failed to get real path of {0}, because {1}", file, error.message());
    return path.str().str();
}

}  // namespace path

namespace fs {

using namespace llvm::sys::fs;

inline std::string resource_dir = "";

inline std::expected<void, std::string> init_resource_dir(llvm::StringRef execute) {
    llvm::SmallString<128> path;
    path::append(path, path::parent_path(execute), "..");
    path::append(path, "lib", "clang", "21");
    if(auto error = real_path(path, path)) {
        return std::unexpected(std::format("{}:{}", error, path.str()));
    }
    resource_dir = path.str();
    return std::expected<void, std::string>();
}

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

inline std::string toURI(llvm::StringRef fspath) {
    if(!path::is_absolute(fspath))
        std::abort();

    llvm::SmallString<128> path("file://");
#if defined(_WIN32)
    path.append("/");
#endif

    for(auto c: fspath) {
        if(c == '\\') {
            path.push_back('/');
        } else if(std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '/') {
            path.push_back(c);
        } else {
            path.push_back('%');
            path.push_back(llvm::hexdigit(c >> 4));
            path.push_back(llvm::hexdigit(c & 0xF));
        }
    }

    return path.str().str();
}

inline std::string decodePercent(llvm::StringRef content) {
    std::string result;
    result.reserve(content.size());

    for(auto iter = content.begin(), send = content.end(); iter != send; ++iter) {
        auto c = *iter;
        if(c == '%' && iter + 2 < send) {
            auto m = *(iter + 1);
            auto n = *(iter + 2);
            if(llvm::isHexDigit(m) && llvm::isHexDigit(n)) {
                result += llvm::hexFromNibbles(m, n);
                iter += 2;
                continue;
            }
        }
        result += c;
    }
    return result;
}

inline std::string toPath(llvm::StringRef uri) {
    llvm::StringRef cloned = uri;

#if defined(_WIN32)
    if(cloned.starts_with("file:///")) {
        cloned = cloned.drop_front(8);
    } else {
        std::abort();
    }
#elif defined(__unix__) || defined(__APPLE__)
    if(cloned.starts_with("file://")) {
        cloned = cloned.drop_front(7);
    } else {
        std::abort();
    }
#else
#error "Unsupported platform"
#endif

    auto decoded = decodePercent(cloned);

    llvm::SmallString<128> result;
    if(auto err = fs::real_path(decoded, result)) {
        std::println("Failed to get real path: {}, Input is {}\n", err.message(), decoded);
        std::abort();
    }

    return result.str().str();
}

}  // namespace fs

namespace vfs = llvm::vfs;

class ThreadSafeFS : public vfs::ProxyFileSystem {
public:
    explicit ThreadSafeFS() : ProxyFileSystem(vfs::createPhysicalFileSystem()) {}

    class VolatileFile : public vfs::File {
    public:
        VolatileFile(std::unique_ptr<vfs::File> Wrapped) : wrapped(std::move(Wrapped)) {
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
        if(!file)
            return file;
        // Try to guess preamble files, they can be memory-mapped even on Windows as
        // clangd has exclusive access to those and nothing else should touch them.
        llvm::StringRef filename = path::filename(Path);
        if(filename.starts_with("preamble-") && filename.ends_with(".pch")) {
            return file;
        }
        return std::make_unique<VolatileFile>(std::move(*file));
    }
};

}  // namespace clice

#include "Async/FileSystem.h"
#include "Support/Logger.h"

namespace clice::async::awaiter {}

namespace clice::async::fs {

namespace awaiter {

template <typename Derived, typename Ret = void>
struct fs : async::awaiter::uv<fs<Derived, Ret>, uv_fs_t, Ret> {
    int start(auto callback) {
        return static_cast<Derived*>(this)->start(callback);
    }

    void cleanup() {
        if(this->request.result < 0) {
            this->error = this->request.result;
        }

        uv_fs_req_cleanup(&this->request);
    }

    auto result() {
        return static_cast<Derived*>(this)->result();
    }
};

struct open : fs<open, handle> {
    const char* path;
    int flags;

    int start(uv_fs_cb cb) {
        /// `uv_fs_open` will copy the path, so we don't need to worry about the
        /// lifetime of the path.
        return uv_fs_open(async::loop, &request, path, flags, 0666, cb);
    }

    auto result() {
        return handle{static_cast<uv_file>(request.result)};
    }
};

struct read : fs<read, ssize_t> {
    uv_file file;
    uv_buf_t bufs[1];

    int start(uv_fs_cb cb) {
        return uv_fs_read(async::loop, &request, file, bufs, 1, -1, cb);
    }

    auto result() {
        return request.result;
    }
};

struct write : fs<write> {
    uv_file file;
    uv_buf_t bufs[1];

    int start(uv_fs_cb cb) {
        return uv_fs_write(async::loop, &request, file, bufs, 1, 0, cb);
    }
};

struct stat : fs<stat, Stats> {
    const char* path;

    int start(uv_fs_cb cb) {
        return uv_fs_stat(async::loop, &request, path, cb);
    }

    auto result() {
        Stats stats;
        stats.mtime = std::chrono::milliseconds(request.statbuf.st_mtim.tv_sec * 1000);
        return stats;
    }
};

}  // namespace awaiter

static int transformFlags(Mode mode) {
    int flags = 0;

    if(mode & Mode::Read) {
        flags |= O_RDONLY;
    }

    if(mode & Mode::Write) {
        flags |= O_WRONLY;
    }

    if(mode & Mode::ReadWrite) {
        flags |= O_RDWR;
    }

    if(mode & Mode::Create) {
        flags |= O_CREAT;
    }

    if(mode & Mode::Append) {
        flags |= O_APPEND;
    }

    if(mode & Mode::Truncate) {
        flags |= O_TRUNC;
    }

    if(mode & Mode::Exclusive) {
        flags |= O_EXCL;
    }

    return flags;
}

handle::~handle() {
    if(file == -1) {
        return;
    }

    uv_fs_t request;
    int error = uv_fs_close(async::loop, &request, file, nullptr);
    if(error < 0) {
        log::warn("Failed to close file: {}", uv_strerror(error));
    }
    uv_fs_req_cleanup(&request);
}

Result<handle> open(std::string path, Mode mode) {
    co_return co_await awaiter::open{
        .path = path.c_str(),
        .flags = transformFlags(mode),
    };
}

Result<ssize_t> read(const handle& handle, char* buffer, std::size_t size) {
    co_return co_await awaiter::read{
        .file = handle.value(),
        .bufs = {uv_buf_init(buffer, size)},
    };
}

Result<std::string> read(std::string path, Mode mode) {
    /// Open the file.
    auto file = co_await open(path, mode);
    if(!file) {
        co_return std::unexpected(file.error());
    }

    /// Read the file content.
    std::string content;

    char buffer[4096];
    while(true) {
        auto result = co_await read(*file, buffer, sizeof(buffer));
        if(!result) {
            co_return std::unexpected(result.error());
        }

        /// FIXME: Move this to awaiter.
        if(*result < 0) {
            co_return std::unexpected(std::error_code(*result, async::category()));
        }

        if(*result == 0) {
            break;
        }

        content.append(buffer, *result);
    }

    co_return content;
}

Result<void> write(const handle& handle, char* buffer, std::size_t size) {
    co_return co_await awaiter::write{
        .file = handle.value(),
        .bufs = {uv_buf_init(buffer, size)},
    };
}

Result<void> write(std::string path, char* buffer, std::size_t size, Mode mode) {
    auto file = co_await open(path, mode);
    if(!file) {
        co_return std::unexpected(file.error());
    }

    if(auto result = co_await write(*file, buffer, size); !result) {
        co_return std::unexpected(result.error());
    }

    co_return std::expected<void, std::error_code>();
}

Result<Stats> stat(std::string path) {
    co_return co_await awaiter::stat{.path = path.c_str()};
}

}  // namespace clice::async::fs


#pragma once

#include <chrono>
#include <cstddef>

#include "Awaiter.h"
#include "Task.h"
#include "libuv.h"
#include "Support/Enum.h"
#include "Support/JSON.h"

#include "llvm/ADT/FunctionExtras.h"
#include "llvm/ADT/StringRef.h"

namespace clice::async {

namespace fs {

struct Mode : refl::Enum<Mode, true> {
    enum Kind {
        /// Open the file for reading.
        Read = 0,

        /// Open the file for writing.
        Write,

        /// Open the file for reading and writing.
        ReadWrite,

        /// If the file does not exist, create it.
        Create,

        /// If the file exists, append the data to the end of the file.
        Append,

        /// If the file exists, truncate the file to zero length.
        Truncate,

        /// If the file exists, fail the open.
        Exclusive,
    };

    using Enum::Enum;
};

struct handle {
public:
    handle(uv_file file) : file(file) {}

    handle(const handle&) = delete;

    handle(handle&& other) noexcept : file(other.file) {
        other.file = -1;
    }

    ~handle();

    handle& operator=(const handle&) = delete;

    handle& operator=(handle&& other) noexcept = delete;

    int value() const {
        return file;
    }

private:
    uv_file file;
};

/// Open the file asynchronously.
Result<handle> open(std::string path, Mode mode);

/// Read the file asynchronously, make sure the buffer is valid until the task is done.
Result<ssize_t> read(const handle& handle, char* buffer, std::size_t size);

Result<std::string> read(std::string path, Mode mode = Mode::Read);

/// Write the file asynchronously, make sure the buffer is valid until the task is done.
Result<void> write(const handle& handle, char* buffer, std::size_t size);

Result<void> write(std::string path,
                   char* buffer,
                   std::size_t size,
                   Mode mode = Mode(Mode::Write, Mode::Create, Mode::Truncate));

struct Stats {
    std::chrono::milliseconds mtime;
    size_t size;
};

Result<Stats> stat(std::string path);

}  // namespace fs

}  // namespace clice::async

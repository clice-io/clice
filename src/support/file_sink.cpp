#include "support/file_sink.h"

#include <cerrno>
#include <format>

#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Process.h"

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace clice::logging {

FileSink::FileSink(int fd, bool owned) : fd(fd), owned(owned) {}

FileSink::~FileSink() {
    if(owned) {
        llvm::sys::Process::SafelyCloseFileDescriptor(fd);
    }
}

std::expected<std::shared_ptr<FileSink>, std::error_code> FileSink::open(llvm::StringRef path) {
    int fd = -1;
    if(auto error = llvm::sys::fs::openFileForWrite(path,
                                                    fd,
                                                    llvm::sys::fs::CD_OpenAlways,
                                                    llvm::sys::fs::OF_Append)) {
        return std::unexpected(error);
    }
    return std::make_shared<FileSink>(fd, true);
}

bool FileSink::write_all(llvm::StringRef text) {
    std::size_t written = 0;
    while(written < text.size()) {
#ifdef _WIN32
        int n =
            ::_write(fd, text.data() + written, static_cast<unsigned int>(text.size() - written));
#else
        ssize_t n = ::write(fd, text.data() + written, text.size() - written);
        if(n < 0 && errno == EINTR) {
            continue;
        }
#endif
        if(n < 0) {
            last_error = std::error_code(errno, std::generic_category());
            torn = torn || written > 0;
            return false;
        }
        written += static_cast<std::size_t>(n);
    }
    torn = false;
    return true;
}

bool FileSink::report_gap() {
    if(dropped_unreported == 0) {
        return true;
    }
    auto note = std::format("{}[logging] dropped {} line(s): {}\n",
                            torn ? "\n" : "",
                            dropped_unreported,
                            last_error.message());
    if(!write_all(note)) {
        return false;
    }
    dropped_unreported = 0;
    return true;
}

void FileSink::sink_it_(const spdlog::details::log_msg& msg) {
    spdlog::memory_buf_t formatted;
    formatter_->format(msg, formatted);
    if(report_gap() && write_all(llvm::StringRef(formatted.data(), formatted.size()))) {
        return;
    }
    dropped_total.fetch_add(1, std::memory_order_relaxed);
    dropped_unreported += 1;
}

}  // namespace clice::logging

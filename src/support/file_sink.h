#pragma once

#include <atomic>
#include <cstddef>
#include <expected>
#include <memory>
#include <mutex>
#include <system_error>

#include "spdlog/sinks/base_sink.h"
#include "llvm/ADT/StringRef.h"

namespace clice::logging {

/// A sink that writes each line straight through to a file descriptor —
/// the session log file, or stderr in every process but the serving
/// master (whose stderr reader is an editor, see StderrSink). Writes
/// block, as any command-line tool's do, but never take the process down:
/// spdlog's own file and console sinks abort on a failed write (exceptions
/// are compiled out), and a full disk under the log directory or a reader
/// that went away (`clice … 2>&1 | head`) is no reason to die. A line that
/// cannot be written is dropped, and the count with the error is written
/// ahead of the first line that gets through again.
class FileSink final : public spdlog::sinks::base_sink<std::mutex> {
public:
    /// Writes to `fd`, closing it on destruction when `owned`.
    FileSink(int fd, bool owned);

    ~FileSink() override;

    /// A sink appending to `path`, or why the file cannot be opened.
    static std::expected<std::shared_ptr<FileSink>, std::error_code> open(llvm::StringRef path);

    /// Lines dropped so far because a write failed.
    std::size_t dropped() const {
        return dropped_total.load(std::memory_order_relaxed);
    }

protected:
    void sink_it_(const spdlog::details::log_msg& msg) override;

    /// Every line is written through; nothing is buffered.
    void flush_() override {}

private:
    /// Writes all of `text`; false when a write fails, with the error kept
    /// for the gap report.
    bool write_all(llvm::StringRef text);

    /// Writes the pending gap report, if any; false when it failed too.
    bool report_gap();

    int fd;
    bool owned;
    std::atomic<std::size_t> dropped_total = 0;
    std::size_t dropped_unreported = 0;
    std::error_code last_error;
    /// A failed write left part of a line behind and nothing followed it
    /// yet: the gap report starts on a line of its own.
    bool torn = false;
};

}  // namespace clice::logging

#include <csignal>
#include <string>
#include <string_view>

#ifndef _WIN32
#include <sys/resource.h>
#endif

#include "test/temp_dir.h"
#include "test/test.h"
#include "support/file_sink.h"

#include "spdlog/details/log_msg.h"
#include "llvm/Support/FileSystem.h"

namespace clice::testing {

namespace {

// POSIX-only: a full disk is simulated by the file size limit.
#ifndef _WIN32

spdlog::details::log_msg info_msg(std::string_view text) {
    return spdlog::details::log_msg(spdlog::source_loc{},
                                    "test",
                                    spdlog::level::info,
                                    spdlog::string_view_t(text.data(), text.size()));
}

/// Caps the size of every file the process writes, as a full disk does;
/// writes past the cap fail with EFBIG instead of raising SIGXFSZ.
struct FileSizeCap {
    rlimit saved{};
    void (*saved_handler)(int) = nullptr;

    explicit FileSizeCap(rlim_t bytes) {
        ::getrlimit(RLIMIT_FSIZE, &saved);
        saved_handler = std::signal(SIGXFSZ, SIG_IGN);
        rlimit capped = saved;
        capped.rlim_cur = bytes;
        ::setrlimit(RLIMIT_FSIZE, &capped);
    }

    ~FileSizeCap() {
        ::setrlimit(RLIMIT_FSIZE, &saved);
        std::signal(SIGXFSZ, saved_handler);
    }
};

TEST_SUITE(FileSink) {

TEST_CASE(FailedWritesDropLines) {
    TempDir tmp;
    auto log = tmp.path("session.log");
    auto sink = logging::FileSink::open(log);
    ASSERT_TRUE(sink.has_value());
    (*sink)->log(info_msg("before the disk filled"));

    std::uint64_t size = 0;
    ASSERT_FALSE(static_cast<bool>(llvm::sys::fs::file_size(log, size)));
    {
        FileSizeCap cap(size);
        (*sink)->log(info_msg("lost one"));
        (*sink)->log(info_msg("lost two"));
    }
    EXPECT_EQ((*sink)->dropped(), 2u);

    (*sink)->log(info_msg("after space came back"));
    auto text = read_file(log).value_or("");
    EXPECT_TRUE(text.find("lost") == std::string::npos);
    auto note = text.find("[logging] dropped 2 line(s): File too large\n");
    ASSERT_TRUE(note != std::string::npos);
    EXPECT_TRUE(text.find("after space came back") > note);
}

TEST_CASE(TornLineEndsBeforeNote) {
    TempDir tmp;
    auto log = tmp.path("session.log");
    auto sink = logging::FileSink::open(log);
    ASSERT_TRUE(sink.has_value());
    {
        // Room for part of the line only.
        FileSizeCap cap(8);
        (*sink)->log(info_msg("a line longer than eight bytes"));
    }
    (*sink)->log(info_msg("next"));
    auto text = read_file(log).value_or("");
    EXPECT_EQ(text.find("\n[logging] dropped 1 line(s)"), 8u);
}

};  // TEST_SUITE(FileSink)

#endif

}  // namespace

}  // namespace clice::testing

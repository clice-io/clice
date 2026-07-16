#include <string>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "test/temp_dir.h"
#include "test/test.h"
#include "support/stderr_sink.h"

#include "spdlog/details/log_msg.h"

namespace clice::testing {

namespace {

// POSIX-only: the Windows PIPE_NOWAIT path has no unit harness (_pipe
// buffers are not fillable without a reader thread); it is exercised
// end-to-end by the integration flood test on the Windows CI runner.
#ifndef _WIN32

spdlog::details::log_msg info_msg(std::string_view text) {
    return spdlog::details::log_msg(spdlog::source_loc{},
                                    "test",
                                    spdlog::level::info,
                                    spdlog::string_view_t(text.data(), text.size()));
}

struct Pipe {
    int fds[2] = {-1, -1};

    Pipe() {
        [[maybe_unused]] int rc = ::pipe(fds);
    }

    ~Pipe() {
        if(fds[0] >= 0) {
            ::close(fds[0]);
        }
        if(fds[1] >= 0) {
            ::close(fds[1]);
        }
    }

    std::string drain() {
        std::string out;
        char buf[4096];
        int flags = ::fcntl(fds[0], F_GETFL);
        ::fcntl(fds[0], F_SETFL, flags | O_NONBLOCK);
        while(true) {
            auto n = ::read(fds[0], buf, sizeof(buf));
            if(n <= 0) {
                break;
            }
            out.append(buf, static_cast<std::size_t>(n));
        }
        ::fcntl(fds[0], F_SETFL, flags);
        return out;
    }
};

TEST_SUITE(StderrSink) {

TEST_CASE(FullPipeDropsLines) {
    Pipe pipe;
    logging::StderrSink sink(pipe.fds[1]);
    EXPECT_TRUE((::fcntl(pipe.fds[1], F_GETFL) & O_NONBLOCK) != 0);

    // Nobody reads: the pipe (64KB) fills and every further line must be
    // shed on the spot. A blocking regression hangs right here — a clean,
    // attributable failure.
    auto line = std::string(100, 'x');
    for(int i = 0; i < 5000 && sink.dropped() == 0; ++i) {
        sink.log(info_msg(line));
    }
    EXPECT_TRUE(sink.dropped() > 0);
}

TEST_CASE(DrainRecoversAndReports) {
    Pipe pipe;
    logging::StderrSink sink(pipe.fds[1]);

    auto line = std::string(100, 'y');
    for(int i = 0; i < 5000 && sink.dropped() == 0; ++i) {
        sink.log(info_msg(line));
    }
    ASSERT_TRUE(sink.dropped() > 0);

    // The client starts reading again: the next line must be preceded by
    // the gap report. Everything is synchronous — no waits involved.
    pipe.drain();
    sink.log(info_msg("after the flood"));
    auto out = pipe.drain();
    EXPECT_TRUE(out.find("client not draining") != std::string::npos);
    EXPECT_TRUE(out.find("after the flood") != std::string::npos);
}

TEST_CASE(SocketGetsNonblocking) {
    // Supervisors attach stderr to sockets; their drain is just as
    // client-controlled as a pipe's.
    int fds[2] = {-1, -1};
    ASSERT_TRUE(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);

    logging::StderrSink sink(fds[1]);
    EXPECT_TRUE((::fcntl(fds[1], F_GETFL) & O_NONBLOCK) != 0);

    ::close(fds[0]);
    ::close(fds[1]);
}

TEST_CASE(RegularFileStaysBlocking) {
    // Only pipes get the non-blocking treatment: a tty's file description
    // is shared with the parent shell, and regular files cannot exert
    // client-controlled backpressure.
    TempDir tmp;
    tmp.touch("log.txt", "");
    int fd = ::open(tmp.path("log.txt").c_str(), O_WRONLY | O_APPEND);
    ASSERT_TRUE(fd >= 0);

    logging::StderrSink sink(fd);
    EXPECT_TRUE((::fcntl(fd, F_GETFL) & O_NONBLOCK) == 0);

    sink.log(info_msg("to the file"));
    EXPECT_TRUE(sink.dropped() == 0);
    ::close(fd);
}

};  // TEST_SUITE(StderrSink)

#endif

}  // namespace
}  // namespace clice::testing

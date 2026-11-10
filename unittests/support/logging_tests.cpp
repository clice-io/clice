module;

#include <fcntl.h>

#ifdef _WIN32
#include <io.h>

// See cache_store.cpp: windows.h must not spill min/max macros.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

module clice;

import :support.logging;
import :support.version;
import :unittests.utils.temp_dir;
import :unittests.utils;

namespace clice::testing {

namespace {

ZEST_SUITE(Logging) {

ZEST_CASE(VersionInfoSet) {
    // Guards the cmake target-stamping plumbing: an unset CLICE_TARGET_STRING
    // would otherwise only show in crash logs.
    ZEXPECT(!clice::version.empty());
    ZEXPECT(!clice::target.empty());
}

};  // ZEST_SUITE(Logging)

/// `text` as one line: `<` and `>` around it, so that a line cut short
/// shows as its text running into the next one's `<`.
std::string as_line(std::string_view text) {
    return std::format("<{}>\n", text);
}

#ifdef _WIN32

ZEST_SUITE(PipeOutput) {

ZEST_CASE(BusyPipeSwitchesLater) {
    // The byte stands in for mimalloc's newline at process start.
    int fds[2] = {-1, -1};
    ZASSERT(::_pipe(fds, 65536, _O_BINARY) == 0);
    ZASSERT(::_write(fds[1], "\n", 1) == 1);

    auto output = logging::pipe_output({.fd = fds[1]});
    ZEXPECT(output.pipe != logging::PipeSwitch::Refused);
    logging::write(output, as_line("while busy"));
    DWORD unread = 0;
    auto read_end = reinterpret_cast<HANDLE>(::_get_osfhandle(fds[0]));
    ZASSERT(::PeekNamedPipe(read_end, nullptr, 0, nullptr, &unread, nullptr) != 0);
    ZEXPECT(unread == 1u);

    char byte = 0;
    ZASSERT(::_read(fds[0], &byte, 1) == 1);
    logging::write(output, as_line("after the read"));
    ::_close(fds[1]);

    std::string out;
    char buf[4096];
    while(ranges::count(out, '\n') < 2) {
        auto n = ::_read(fds[0], buf, sizeof(buf));
        ZASSERT(n > 0);
        out.append(buf, static_cast<std::size_t>(n));
    }
    ZEXPECT(out.find("while busy") < out.find("after the read"));
    ZEXPECT(output.dropped == 0);
    ::_close(fds[0]);
}

};  // ZEST_SUITE(PipeOutput)

#else

// The rest is POSIX-only: the Windows PIPE_NOWAIT backpressure path has no
// unit harness (_pipe buffers are not fillable without a reader thread);
// it is exercised end-to-end by the integration flood test on the Windows
// CI runner. A full disk is simulated by the file size limit, which
// Windows lacks.

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

ZEST_SUITE(PipeOutput) {

ZEST_CASE(FullPipeDropsLines) {
    Pipe pipe;
    auto output = logging::pipe_output({.fd = pipe.fds[1], .capacity = 4096});
    ZEXPECT((::fcntl(pipe.fds[1], F_GETFL) & O_NONBLOCK) != 0);

    // Nobody reads: the pipe (64KB) and the buffer budget fill, and every
    // further line must evict an oldest one instead of blocking. A
    // blocking regression hangs right here — a clean, attributable
    // failure.
    auto line = std::string(100, 'x');
    for(int i = 0; i < 5000 && output.dropped == 0; i += 1) {
        logging::write(output, as_line(line));
    }
    ZEXPECT(output.dropped > 0);
}

ZEST_CASE(BackpressureBuffersLines) {
    Pipe pipe;
    auto output = logging::pipe_output({.fd = pipe.fds[1]});

    // More than the pipe holds, less than the buffer budget: a reader
    // that is merely slow loses nothing.
    for(int i = 0; i < 600; i += 1) {
        logging::write(output, as_line(std::format("line number {}", i)));
    }
    ZEXPECT(output.dropped == 0);

    auto out = pipe.drain();
    logging::write(output, as_line("the flush trigger"));
    out += pipe.drain();
    ZEXPECT(out.find("line number 0") != std::string::npos);
    ZEXPECT(out.find("line number 599") != std::string::npos);
    ZEXPECT(out.find("the flush trigger") != std::string::npos);
    ZEXPECT(output.dropped == 0);
}

ZEST_CASE(DrainRecoversAndReports) {
    Pipe pipe;
    auto output = logging::pipe_output({.fd = pipe.fds[1], .capacity = 4096});

    auto line = std::string(100, 'y');
    for(int i = 0; i < 5000 && output.dropped == 0; i += 1) {
        logging::write(output, as_line(line));
    }
    ZASSERT(output.dropped > 0);

    // The client starts reading again: one call flushes the buffered
    // survivors, then the gap report — unless the room the full pipe had
    // left took it already — then the fresh line; everything is
    // synchronous, no waits involved.
    auto first = pipe.drain();
    logging::write(output, as_line("after the flood"));
    auto second = pipe.drain();
    // The buffered survivors waited for the room the drain made.
    ZEXPECT(second.find('y') != std::string::npos);
    auto out = first + second;
    ZEXPECT(out.find("client not draining") != std::string::npos);
    ZEXPECT(out.find("after the flood") != std::string::npos);
    ZEXPECT(out.find("client not draining") < out.find("after the flood"));
}

#ifdef F_SETPIPE_SZ
ZEST_CASE(TinyPipeStillReports) {
    // Windows/macOS pipes are far smaller than Linux's 64KB, so a flushed
    // backlog takes many calls to deliver — the gap report must ride the
    // FIRST accepted quantum, not wait for the backlog to clear (that
    // ordering bug only showed up on small-pipe CI runners).
    Pipe pipe;
    ::fcntl(pipe.fds[1], F_SETPIPE_SZ, 4096);
    auto output = logging::pipe_output({.fd = pipe.fds[1], .capacity = 65536});

    auto line = std::string(100, 'z');
    for(int i = 0; i < 5000 && output.dropped == 0; i += 1) {
        logging::write(output, as_line(line));
    }
    ZASSERT(output.dropped > 0);

    // Drain only one tiny pipe's worth, log once: the report must already
    // be in that first quantum even though most of the backlog is not.
    pipe.drain();
    logging::write(output, as_line("nudge"));
    auto out = pipe.drain();
    ZEXPECT(out.find("client not draining") != std::string::npos);
    // Eviction must never cut the tail off a partially written line: a
    // torn line shows as its text running into the next line's start.
    ZEXPECT(out.find("z<") == std::string::npos);
}

ZEST_CASE(TornLineEndsBeforeNote) {
    // The pipe takes the head of a long line; lines after it overflow the
    // backlog. The line's tail goes out before the gap report does.
    Pipe pipe;
    ::fcntl(pipe.fds[1], F_SETPIPE_SZ, 4096);
    auto output = logging::pipe_output({.fd = pipe.fds[1], .capacity = 8192});
    auto long_line = as_line(std::string(100'000, 'x'));
    logging::write(output, long_line);
    for(int i = 0; i < 5000 && output.dropped == 0; i += 1) {
        logging::write(output, as_line(std::string(100, 'y')));
    }
    ZASSERT(output.dropped > 0);

    std::string out;
    for(int i = 0; i < 100; i += 1) {
        out += pipe.drain();
        logging::write(output, as_line("nudge"));
    }
    out += pipe.drain();
    ZEXPECT(out.find(long_line) != std::string::npos);
    ZEXPECT(out.find("client not draining") != std::string::npos);
    ZEXPECT(out.find("client not draining") > out.find(long_line));
}

ZEST_CASE(NoteSurvivesPressure) {
    // The gap report lives outside the backlog: pressure that keeps
    // evicting buffered lines must never evict the count itself.
    Pipe pipe;
    ::fcntl(pipe.fds[1], F_SETPIPE_SZ, 4096);
    auto output = logging::pipe_output({.fd = pipe.fds[1], .capacity = 8192});

    auto line = std::string(100, 'w');
    for(int i = 0; i < 5000 && output.dropped == 0; i += 1) {
        logging::write(output, as_line(line));
    }
    ZASSERT(output.dropped > 0);
    auto seen = output.dropped;

    // Keep the flood going well past several full buffer turnovers.
    for(int i = 0; i < 500; i += 1) {
        logging::write(output, as_line(line));
    }
    ZEXPECT(output.dropped > seen);

    pipe.drain();
    logging::write(output, as_line("nudge"));
    auto out = pipe.drain();
    ZEXPECT(out.find("client not draining") != std::string::npos);
}
#endif

ZEST_CASE(SocketGetsNonblocking) {
    // Supervisors attach stderr to sockets; their drain is just as
    // client-controlled as a pipe's.
    int fds[2] = {-1, -1};
    ZASSERT(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);

    auto output = logging::pipe_output({.fd = fds[1]});
    ZEXPECT((::fcntl(fds[1], F_GETFL) & O_NONBLOCK) != 0);

    ::close(fds[0]);
    ::close(fds[1]);
}

ZEST_CASE(RegularFileStaysBlocking) {
    // Only pipes get the non-blocking treatment: a tty's file description
    // is shared with the parent shell, and regular files cannot exert
    // client-controlled backpressure.
    TempDir tmp;
    tmp.touch("log.txt", "");
    int fd = ::open(tmp.path("log.txt").c_str(), O_WRONLY | O_APPEND);
    ZASSERT(fd >= 0);

    auto output = logging::pipe_output({.fd = fd});
    ZEXPECT((::fcntl(fd, F_GETFL) & O_NONBLOCK) == 0);

    logging::write(output, as_line("to the file"));
    ZEXPECT(output.dropped == 0);
    ::close(fd);
}

};  // ZEST_SUITE(PipeOutput)

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

/// A FileOutput appending to the file at `path`.
logging::FileOutput append_to(const std::string& path) {
    return {.fd = ::open(path.c_str(), O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0644)};
}

ZEST_SUITE(FileOutput) {

ZEST_CASE(FailedWritesDropLines) {
    TempDir tmp;
    auto log = tmp.path("session.log");
    auto output = append_to(log);
    ZASSERT(output.fd >= 0);
    logging::write(output, as_line("before the disk filled"));

    auto status = fs::status(log);
    ZASSERT(status);
    {
        FileSizeCap cap(status->size);
        logging::write(output, as_line("lost one"));
        logging::write(output, as_line("lost two"));
    }
    ZEXPECT(output.dropped == 2u);

    logging::write(output, as_line("after space came back"));
    auto text = read_file(log).value_or("");
    ZEXPECT(text.find("lost") == std::string::npos);
    auto note = text.find("[logging] dropped 2 line(s): File too large\n");
    ZASSERT(note != std::string::npos);
    ZEXPECT(text.find("after space came back") > note);
    ::close(output.fd);
}

// Linux writes up to the size limit and fails the rest; macOS refuses a
// write crossing it whole.
#ifdef __linux__
ZEST_CASE(TornLineGetsNewline) {
    TempDir tmp;
    auto log = tmp.path("session.log");
    auto output = append_to(log);
    ZASSERT(output.fd >= 0);
    {
        FileSizeCap cap(8);
        logging::write(output, as_line("a line longer than eight bytes"));
    }
    logging::write(output, as_line("next"));
    auto text = read_file(log).value_or("");
    ZEXPECT(text.find("\n[logging] dropped 1 line(s)") == 8u);
    ::close(output.fd);
}
#endif

};  // ZEST_SUITE(FileOutput)

#endif

}  // namespace

}  // namespace clice::testing

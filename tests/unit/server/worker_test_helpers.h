#pragma once

#include <csignal>
#include <string>

#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#endif

#include "test/temp_dir.h"
#include "command/argument_parser.h"
#include "command/command.h"
#include "server/protocol.h"
#include "support/filesystem.h"

#include "kota/async/async.h"
#include "kota/ipc/codec/bincode.h"
#include "kota/ipc/peer.h"
#include "kota/ipc/transport.h"

namespace clice::testing {

namespace {

/// Ignore SIGPIPE so broken pipes from exited workers don't kill the test binary.
struct SigpipeGuard {
    SigpipeGuard() {
#ifndef _WIN32
        std::signal(SIGPIPE, SIG_IGN);
#endif
    }
};

static SigpipeGuard sigpipe_guard;

/// Resolve path to the clice binary for spawning workers.
inline std::string clice_binary() {
    auto res_dir = resource_dir();
    // res_dir is <build>/lib/clang/...
    // clice binary is at <build>/bin/clice
    auto build_dir = llvm::sys::path::parent_path(
        llvm::sys::path::parent_path(llvm::sys::path::parent_path(res_dir)));
    llvm::SmallString<256> path(build_dir);
    llvm::sys::path::append(path, "bin", "clice");
    return std::string(path);
}

/// Build compile arguments for a source file, including -resource-dir.
inline std::vector<std::string> make_args(const std::string& file_path,
                                          const std::string& extra = "") {
    std::vector<std::string> args =
        {"clang++", "-fsyntax-only", "-resource-dir", std::string(resource_dir()), "-c", file_path};
    if(!extra.empty()) {
        args.insert(args.begin() + 1, extra);
    }
    return args;
}

/// Helper: spawn a worker process and return a BincodePeer connected to it.
struct WorkerHandle {
    kota::event_loop loop;
    kota::process proc{};
    std::unique_ptr<kota::ipc::StreamTransport> transport;
    std::unique_ptr<kota::ipc::BincodePeer> peer;
    int stderr_fd = -1;

    bool spawn(const std::string& mode, std::uint64_t memory_limit = 0) {
        auto binary = clice_binary();

#ifndef _WIN32
        // Redirect worker stderr to a temp file for debugging.
        std::string stderr_path = "/tmp/clice_worker_stderr_" + mode + ".log";
        stderr_fd = ::open(stderr_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
#endif

        kota::process::options opts;
        opts.file = binary;
        opts.args = {binary, "--mode", mode};
        if(memory_limit > 0) {
            opts.args.push_back("--worker-memory-limit");
            opts.args.push_back(std::to_string(memory_limit));
        }
        opts.streams = {
            kota::process::stdio::pipe(true, false),  // stdin: child reads
            kota::process::stdio::pipe(false, true),  // stdout: child writes
            stderr_fd >= 0 ? kota::process::stdio::from_fd(stderr_fd)
                           : kota::process::stdio::ignore(),
        };

        auto result = kota::process::spawn(opts, loop);
        if(!result) {
#ifndef _WIN32
            if(stderr_fd >= 0)
                ::close(stderr_fd);
#endif
            return false;
        }

        auto& spawn = *result;
        transport = std::make_unique<kota::ipc::StreamTransport>(std::move(spawn.stdout_pipe),
                                                                 std::move(spawn.stdin_pipe));
        peer = std::make_unique<kota::ipc::BincodePeer>(loop, std::move(transport));
        proc = std::move(spawn.proc);
#ifndef _WIN32
        if(stderr_fd >= 0)
            ::close(stderr_fd);
#endif
        return true;
    }

    /// Run a coroutine on the event loop and return when it completes.
    template <typename F>
    void run(F&& coro_factory) {
        loop.schedule(peer->run());
        loop.schedule(coro_factory());
        loop.run();
    }
};

}  // namespace

}  // namespace clice::testing

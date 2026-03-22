#pragma once

#include <csignal>
#include <fcntl.h>
#include <fstream>
#include <memory>
#include <string>

#include "eventide/async/io/loop.h"
#include "eventide/async/io/process.h"
#include "eventide/ipc/peer.h"
#include "eventide/ipc/transport.h"
#include "server/protocol.h"
#include "support/filesystem.h"

namespace clice::testing {

namespace {

/// Ignore SIGPIPE so broken pipes from exited workers don't kill the test binary.
struct SigpipeGuard {
    SigpipeGuard() {
        std::signal(SIGPIPE, SIG_IGN);
    }
};

static SigpipeGuard sigpipe_guard;

namespace et = eventide;

/// Resolve path to the clice binary for spawning workers.
inline std::string clice_binary() {
    auto resource_dir = fs::resource_dir;
    // resource_dir is <build>/lib/clang/...
    // clice binary is at <build>/bin/clice
    auto build_dir = llvm::sys::path::parent_path(
        llvm::sys::path::parent_path(llvm::sys::path::parent_path(resource_dir)));
    llvm::SmallString<256> path(build_dir);
    llvm::sys::path::append(path, "bin", "clice");
    return std::string(path);
}

/// RAII temporary file: writes content to disk, removes on destruction.
struct TempFile {
    std::string path;

    TempFile(const std::string& name, const std::string& content) {
        path = "/tmp/clice_test_" + name;
        std::ofstream ofs(path);
        ofs << content;
    }

    ~TempFile() {
        std::remove(path.c_str());
    }

    std::string uri() const {
        return "file://" + path;
    }

    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;
};

/// Build compile arguments for a source file, including -resource-dir.
inline std::vector<std::string> make_args(const std::string& file_path,
                                          const std::string& extra = "") {
    std::vector<std::string> args =
        {"clang++", "-fsyntax-only", "-resource-dir", fs::resource_dir, "-c", file_path};
    if(!extra.empty()) {
        args.insert(args.begin() + 1, extra);
    }
    return args;
}

/// Helper: spawn a worker process and return a BincodePeer connected to it.
struct WorkerHandle {
    et::event_loop loop;
    et::process proc{};
    std::unique_ptr<et::ipc::StreamTransport> transport;
    std::unique_ptr<et::ipc::BincodePeer> peer;
    int stderr_fd = -1;

    bool spawn(const std::string& mode, std::uint64_t memory_limit = 0) {
        auto binary = clice_binary();

        // Redirect worker stderr to a temp file for debugging.
        std::string stderr_path = "/tmp/clice_worker_stderr_" + mode + ".log";
        stderr_fd = ::open(stderr_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);

        et::process::options opts;
        opts.file = binary;
        opts.args = {binary, "--mode", mode};
        if(memory_limit > 0) {
            opts.args.push_back("--worker-memory-limit");
            opts.args.push_back(std::to_string(memory_limit));
        }
        opts.streams = {
            et::process::stdio::pipe(true, false),  // stdin: child reads
            et::process::stdio::pipe(false, true),  // stdout: child writes
            stderr_fd >= 0 ? et::process::stdio::from_fd(stderr_fd) : et::process::stdio::ignore(),
        };

        auto result = et::process::spawn(opts, loop);
        if(!result)
            return false;

        auto& spawn = *result;
        transport = std::make_unique<et::ipc::StreamTransport>(std::move(spawn.stdout_pipe),
                                                               std::move(spawn.stdin_pipe));
        peer = std::make_unique<et::ipc::BincodePeer>(loop, std::move(transport));
        proc = std::move(spawn.proc);
        if(stderr_fd >= 0)
            ::close(stderr_fd);
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

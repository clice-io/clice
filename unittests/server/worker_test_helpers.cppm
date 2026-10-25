module;

#include "support/prelude.h"

#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#endif

module clice:unittests.server.worker_test_helpers;

import :command.table;
import :support.process;
import :unittests.utils.temp_dir;
import :vfs.path;
import :worker.probe;
import :worker.protocol;

namespace clice::testing {

/// Resolve path to the clice binary for spawning workers.
inline std::string clice_binary() {
    auto res_dir = resource_dir();
    // res_dir is <build>/lib/clang/...
    // clice binary is at <build>/bin/clice
    auto build_dir = path::parent(path::parent(path::parent(res_dir)));
    auto binary = path::join(build_dir, "bin", "clice");
#ifdef _WIN32
    binary += ".exe";
#endif
    return binary;
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

/// Returns once a worker parked the work behind `probe`'s gate `id`.
inline kota::task<> parked(BuildProbe& probe, std::uint64_t id) {
    kota::event held;
    auto connection = probe.on_held.connect([&](std::uint64_t each) {
        if(each == id) {
            held.set();
        }
    });
    if(ranges::none_of(probe.gates(), [&](const BuildProbe::Gate& gate) {
           return gate.id == id && gate.parked;
       })) {
        co_await held.wait();
    }
}

/// Helper: spawn a worker process and return a BincodePeer connected to it.
struct WorkerHandle : kota::zest::LoopFixture {
    kota::process proc{};
    std::unique_ptr<ipc::StreamTransport> transport;
    std::unique_ptr<ipc::BincodePeer> peer;
    int stderr_fd = -1;

    /// Changes to the environment the worker inherits.
    std::vector<kota::process::env_change> env;

    /// The gates placed, and of them the ones the worker parked at.
    std::uint64_t gates = 0;
    llvm::DenseSet<std::uint64_t> parked_gates;
    kota::event gate_parked{false};

    WorkerHandle() {
        // Hang detection: outlasts the 30s bound the cancellation tests put
        // on a request.
        watchdog = std::chrono::seconds(45);
    }

    bool spawn(bool stateful = false, std::size_t max_documents = 0) {
        auto binary = clice_binary();
        auto label = stateful ? "stateful" : "stateless";

#ifndef _WIN32
        std::string stderr_path = std::string("/tmp/clice_worker_stderr_") + label + ".log";
        stderr_fd = ::open(stderr_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
#endif

        kota::process::options opts;
        opts.program = binary;
        opts.args = {"worker"};
        if(stateful) {
            opts.args.push_back("--stateful");
        }
        if(max_documents > 0) {
            opts.args.push_back("--max-documents");
            opts.args.push_back(std::to_string(max_documents));
        }
        opts.env_changes = env;
        opts.creation.process_group = true;
        opts.creation.exit_with_parent = can_exit_with_parent;
        opts.streams = {
            kota::process::stdio::pipe(),
            kota::process::stdio::pipe(),
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
        transport = std::make_unique<ipc::StreamTransport>(std::move(spawn.stdout_pipe),
                                                           std::move(spawn.stdin_pipe));
        peer = std::make_unique<ipc::BincodePeer>(loop, std::move(transport));
        peer->on_notification([this](const worker::GateParkedParams& params) {
            parked_gates.insert(params.id);
            gate_parked.set();
        });
        proc = std::move(spawn.proc);
#ifndef _WIN32
        if(stderr_fd >= 0)
            ::close(stderr_fd);
#endif
        return true;
    }

    /// Park the work of the next request like `params` where it starts in
    /// the worker, as the master's BuildProbe does; returns the gate's id.
    template <typename Params>
    std::uint64_t gate(const Params& params) {
        gates += 1;
        peer->send_notification(worker::GateParams{.id = gates, .tag = worker::crash_tag(params)});
        return gates;
    }

    /// Returns once the worker parked the work behind gate `id`.
    kota::task<> parked(std::uint64_t id) {
        while(!parked_gates.contains(id)) {
            gate_parked.reset();
            co_await gate_parked.wait();
        }
    }

    /// Let the work parked behind gate `id` go on.
    void release(std::uint64_t id) {
        peer->send_notification(worker::GateReleaseParams{id});
    }

    /// Run a coroutine on the event loop and return once the worker has
    /// exited. Closes the worker's input afterwards — including when the
    /// body unwound early through a failed CO_ASSERT — so the IO pump
    /// drains and a failing test reports instead of hanging the suite.
    template <typename F>
    void run(F&& coro_factory) {
        auto body = [](WorkerHandle& self, F factory) -> kota::task<> {
            co_await factory();
            self.peer->close_output();
        };
        LoopFixture::run(peer->run(), body(*this, std::forward<F>(coro_factory)), proc.wait());
    }
};

}  // namespace clice::testing

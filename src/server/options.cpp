#include "server/options.h"

#include <algorithm>
#include <cstring>
#include <thread>

#include "support/logging.h"

#ifdef __APPLE__
#include <mach-o/dyld.h>
#elif defined(__linux__)
#include <unistd.h>
#elif defined(_WIN32)
#include <windows.h>
#endif

namespace clice {

static std::string get_self_path(const char* argv0) {
#ifdef __APPLE__
    char buf[4096];
    uint32_t size = sizeof(buf);
    if(_NSGetExecutablePath(buf, &size) == 0) {
        char resolved[PATH_MAX];
        if(realpath(buf, resolved)) {
            return resolved;
        }
        return buf;
    }
#elif defined(__linux__)
    char buf[4096];
    ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if(len > 0) {
        buf[len] = '\0';
        return buf;
    }
#elif defined(_WIN32)
    char buf[MAX_PATH];
    if(GetModuleFileNameA(nullptr, buf, MAX_PATH)) {
        return buf;
    }
#endif
    return argv0;
}

Options Options::parse(int argc, const char** argv) {
    Options opts;
    opts.self_path = get_self_path(argv[0]);

    for(int i = 1; i < argc; ++i) {
        std::string_view arg = argv[i];

        if(arg == "--help" || arg == "-h") {
            LOG_INFO("Usage: clice [options]");
            LOG_INFO("  --mode=pipe|socket|stateless-worker|stateful-worker");
            LOG_INFO("  --host=<ip>            (default: 127.0.0.1)");
            LOG_INFO("  --port=<port>          (default: 50051)");
            LOG_INFO("  --worker-memory-limit=<bytes>");
            std::exit(0);
        }

        auto extract_value = [&](std::string_view prefix) -> std::string_view {
            if(arg.starts_with(prefix)) {
                return arg.substr(prefix.size());
            }
            return {};
        };

        if(auto v = extract_value("--mode="); !v.empty()) {
            if(v == "pipe") {
                opts.mode = Mode::Pipe;
            } else if(v == "socket") {
                opts.mode = Mode::Socket;
            } else if(v == "stateless-worker") {
                opts.mode = Mode::StatelessWorker;
            } else if(v == "stateful-worker") {
                opts.mode = Mode::StatefulWorker;
            } else {
                LOG_WARN("Unknown mode: {}", v);
            }
        } else if(auto v = extract_value("--host="); !v.empty()) {
            opts.host = std::string(v);
        } else if(auto v = extract_value("--port="); !v.empty()) {
            opts.port = std::atoi(v.data());
        } else if(auto v = extract_value("--worker-memory-limit="); !v.empty()) {
            opts.worker_memory_limit = std::stoull(std::string(v));
        } else if(arg == "--workers") {
            opts.enable_workers = true;
        }
    }

    return opts;
}

void Options::compute_defaults() {
    auto cores = std::thread::hardware_concurrency();
    if(cores == 0)
        cores = 4;

    if(stateless_worker_count == 0) {
        stateless_worker_count = std::max(1u, cores / 2);
    }

    if(stateful_worker_count == 0) {
        stateful_worker_count = std::max(1u, cores / 4);
    }

    if(worker_memory_limit == 0) {
        worker_memory_limit = 4ULL * 1024 * 1024 * 1024;
    }
}

}  // namespace clice

#include <csignal>
#include <cstdint>
#include <print>
#include <sstream>
#include <string>

#include "server/transport/agentic.h"
#include "server/transport/master_server.h"
#include "server/worker/stateful_worker.h"
#include "server/worker/stateless_worker.h"
#include "support/logging.h"

#include "kota/deco/deco.h"

namespace clice {

namespace deco = kota::deco;
using deco::decl::KVStyle;

struct LintOptions {
    DecoFlag(names = {"-h", "--help"}, help = "Show help", required = false)
    help;
};

struct FormatOptions {
    DecoFlag(names = {"-h", "--help"}, help = "Show help", required = false)
    help;
};

struct WorkerOptions {
    DecoFlag(names = {"-h", "--help"}, help = "Show help", required = false)
    help;

    DecoFlag(names = {"--stateful"},
             help = "Run as stateful worker (default: stateless)",
             required = false)
    stateful;

    DecoKV(style = KVStyle::JoinedOrSeparate,
           names = {"--memory-limit", "--memory-limit="},
           help = "Memory limit in bytes (stateful worker only)",
           required = false)
    <std::uint64_t> memory_limit;

    DecoKV(style = KVStyle::JoinedOrSeparate,
           names = {"--worker-name", "--worker-name="},
           required = false)
    <std::string> worker_name;

    DecoKV(style = KVStyle::JoinedOrSeparate, names = {"--log-dir", "--log-dir="}, required = false)
    <std::string> log_dir;
};

bool apply_log_level(const std::string& level_str) {
    auto level = spdlog::level::from_str(level_str);
    if(level == spdlog::level::off && level_str != "off") {
        std::println(stderr,
                     "unknown log level '{}', valid: trace, debug, info, warn, error, off",
                     level_str);
        return false;
    }
    logging::options.level = level;
    return true;
}

template <typename T>
void print_usage(T& cmd) {
    std::ostringstream ss;
    cmd.usage(ss);
    std::print("{}", ss.str());
}

}  // namespace clice

int main(int argc, const char** argv) {
#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN);
#endif

    namespace deco = kota::deco;

    auto args = deco::util::argvify(argc, argv);
    const char* self_path = argv[0];

    int exit_code = 1;

    auto serve_cmd = deco::cli::command<clice::ServerOptions>("clice serve [OPTIONS]");
    serve_cmd
        .matchAll([&](clice::ServerOptions opts) {
            if(opts.help) {
                clice::print_usage(serve_cmd);
                exit_code = 0;
                return;
            }
            if(!clice::apply_log_level(opts.log_level.value_or("info")))
                return;
            exit_code = clice::run_serve_mode(opts, self_path);
        })
        .on_error([](auto err) { LOG_ERROR("{}", err.message); });

    auto query_cmd = deco::cli::command<clice::QueryOptions>("clice query [OPTIONS]");
    query_cmd
        .matchAll([&](clice::QueryOptions opts) {
            if(opts.help) {
                clice::print_usage(query_cmd);
                exit_code = 0;
                return;
            }
            auto port = opts.port.value_or(0);
            if(port <= 0 || port > 65535) {
                LOG_ERROR("--port must be between 1 and 65535");
                return;
            }
            if(!clice::apply_log_level(opts.log_level.value_or("info")))
                return;
            exit_code = clice::run_agentic_mode(opts);
        })
        .on_error([](auto err) { LOG_ERROR("{}", err.message); });

    auto worker_cmd = deco::cli::command<clice::WorkerOptions>("clice worker [OPTIONS]");
    worker_cmd
        .matchAll([&](clice::WorkerOptions opts) {
            if(opts.help) {
                clice::print_usage(worker_cmd);
                exit_code = 0;
                return;
            }
            auto name = opts.worker_name.value_or("worker");
            auto log_dir = opts.log_dir.value_or("");
            if(opts.stateful) {
                auto limit = opts.memory_limit.value_or(4ULL * 1024 * 1024 * 1024);
                exit_code = clice::run_stateful_worker_mode(limit, name, log_dir);
            } else {
                exit_code = clice::run_stateless_worker_mode(name, log_dir);
            }
        })
        .on_error([](auto err) { LOG_ERROR("{}", err.message); });

    auto lint_cmd = deco::cli::command<clice::LintOptions>("clice lint [OPTIONS]");
    lint_cmd
        .matchAll([&](clice::LintOptions opts) {
            if(opts.help) {
                clice::print_usage(lint_cmd);
                exit_code = 0;
                return;
            }
            LOG_ERROR("lint is not yet implemented");
        })
        .on_error([](auto err) { LOG_ERROR("{}", err.message); });

    auto format_cmd = deco::cli::command<clice::FormatOptions>("clice format [OPTIONS]");
    format_cmd
        .matchAll([&](clice::FormatOptions opts) {
            if(opts.help) {
                clice::print_usage(format_cmd);
                exit_code = 0;
                return;
            }
            LOG_ERROR("format is not yet implemented");
        })
        .on_error([](auto err) { LOG_ERROR("{}", err.message); });

    deco::cli::SubCommander clice("clice <command> [<args>]",
                                  "A C++ development toolkit built on LLVM/Clang");

    auto print_root_usage = [&] {
        std::println("usage: clice <command> [<args>]\n");
        clice::print_usage(clice);
    };

    clice.add({.name = "serve", .description = "Start LSP server"}, serve_cmd)
        .add({.name = "query", .description = "Query symbol information from a running server"},
             query_cmd)
        .add({.name = "worker"}, worker_cmd)
        .add({.name = "lint", .description = "Lint C++ source files"}, lint_cmd)
        .add({.name = "format", .description = "Format C++ source files"}, format_cmd)
        .when_err([&](auto err) {
            if(err.type == deco::cli::SubCommandError::Type::MissingSubCommand) {
                print_root_usage();
                exit_code = 0;
            } else {
                LOG_ERROR("{}", err.message);
            }
        });

    if(!args.empty() && (args[0] == "--version" || args[0] == "-v")) {
        std::println("clice version 0.1.0");
        return 0;
    }

    if(!args.empty() && (args[0] == "--help" || args[0] == "-h")) {
        print_root_usage();
        return 0;
    }

    clice(args);
    return exit_code;
}

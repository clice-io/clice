#include <cstdio>
#include <expected>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "eventide/deco/macro.h"
#include "eventide/deco/runtime.h"
#include "eventide/serde/config.h"
#include "server/protocol.h"
#include "server/runtime.h"
#include "support/logging.h"

namespace {

using clice::server::Mode;
using clice::server::Options;
using clice::server::WorkerRole;

struct ModeOption {
    Mode value = Mode::Pipe;

    auto into(std::string_view text) -> std::optional<std::string_view> {
        if(text == "pipe") {
            value = Mode::Pipe;
            return std::nullopt;
        }
        if(text == "socket") {
            value = Mode::Socket;
            return std::nullopt;
        }
        if(text == "worker") {
            value = Mode::Worker;
            return std::nullopt;
        }
        return "invalid --mode, expected: pipe|socket|worker";
    }
};

struct WorkerRoleOption {
    WorkerRole value = WorkerRole::Stateful;

    auto into(std::string_view text) -> std::optional<std::string_view> {
        if(text == "stateful") {
            value = WorkerRole::Stateful;
            return std::nullopt;
        }
        if(text == "stateless") {
            value = WorkerRole::Stateless;
            return std::nullopt;
        }
        return "invalid --worker-role, expected: stateful|stateless";
    }
};

struct CliOptions {
    DECO_CFG_START(required = false;);

    DecoFlag(names = {"-h", "--help"}; help = "Show this help message and exit"; required = false;)
    help = false;

    DecoFlag(names = {std::string_view(clice::server::k_worker_mode)};
             help = "Run as worker process";
             required = false;)
    worker_mode = false;

    DecoKVStyled(deco::decl::KVStyle::Joined, names = {"--mode=", "--mode"}; meta_var = "MODE";
                 help = "Server mode: pipe|socket|worker";
                 required = false;)
    <ModeOption> mode;

    DecoKVStyled(deco::decl::KVStyle::Joined, names = {"--host=", "--host"}; meta_var = "HOST";
                 help = "Socket host (default: 127.0.0.1)";
                 required = false;)
    <std::string> host;

    DecoKVStyled(deco::decl::KVStyle::Joined, names = {"--port=", "--port"}; meta_var = "PORT";
                 help = "Socket port (default: 50051)";
                 required = false)
    <int> port;

    DecoKVStyled(deco::decl::KVStyle::Joined, names = {"--worker-count=", "--worker-count"};
                 meta_var = "N";
                 help = "Worker process count (default: 2)";
                 required = false)
    <std::size_t> worker_count;

    DecoKVStyled(deco::decl::KVStyle::Joined,
                 names = {"--stateless-worker-count=", "--stateless-worker-count"};
                 meta_var = "N";
                 help = "Stateless worker process count (default: 2)";
                 required = false)
    <std::size_t> stateless_worker_count;

    DecoKVStyled(deco::decl::KVStyle::Joined,
                 names = {"--worker-doc-capacity=", "--worker-doc-capacity"};
                 meta_var = "N";
                 help = "Per-worker AST cache capacity (default: 32)";
                 required = false;)
    <std::size_t> worker_document_capacity;

    DecoKVStyled(deco::decl::KVStyle::Joined,
                 names = {"--master-doc-capacity=", "--master-doc-capacity"};
                 meta_var = "N";
                 help = "Master ownership cache capacity (default: 256)";
                 required = false;)
    <std::size_t> master_document_capacity;

    DecoKVStyled(deco::decl::KVStyle::Joined, names = {"--worker-role=", "--worker-role"};
                 meta_var = "ROLE";
                 help = "Worker role: stateful|stateless";
                 required = false;)
    <WorkerRoleOption> worker_role;

    DECO_CFG_END();
};

auto resolve_self_path(int argc, const char** argv) -> std::string {
    if(argc <= 0 || argv == nullptr || argv[0] == nullptr) {
        return "clice";
    }

    std::error_code ec;
    auto absolute = std::filesystem::absolute(argv[0], ec);
    if(ec) {
        return std::string(argv[0]);
    }
    return absolute.string();
}

auto build_options(const CliOptions& cli_options, int argc, const char** argv)
    -> std::expected<Options, std::string> {
    Options options;
    options.self_path = resolve_self_path(argc, argv);

    if(cli_options.mode.value.has_value()) {
        options.mode = cli_options.mode->value;
    } else if(cli_options.worker_mode.value.value_or(false)) {
        options.mode = Mode::Worker;
    }

    if(cli_options.host.value.has_value()) {
        options.host = *cli_options.host;
    }
    if(cli_options.port.value.has_value()) {
        if(*cli_options.port <= 0) {
            return std::unexpected("--port must be a positive integer");
        }
        options.port = *cli_options.port;
    }
    if(cli_options.worker_count.value.has_value()) {
        if(*cli_options.worker_count == 0) {
            return std::unexpected("--worker-count must be a positive integer");
        }
        options.worker_count = *cli_options.worker_count;
    }
    if(cli_options.stateless_worker_count.value.has_value()) {
        if(*cli_options.stateless_worker_count == 0) {
            return std::unexpected("--stateless-worker-count must be a positive integer");
        }
        options.stateless_worker_count = *cli_options.stateless_worker_count;
    }
    if(cli_options.worker_document_capacity.value.has_value()) {
        if(*cli_options.worker_document_capacity == 0) {
            return std::unexpected("--worker-doc-capacity must be a positive integer");
        }
        options.worker_document_capacity = *cli_options.worker_document_capacity;
    }
    if(cli_options.master_document_capacity.value.has_value()) {
        if(*cli_options.master_document_capacity == 0) {
            return std::unexpected("--master-doc-capacity must be a positive integer");
        }
        options.master_document_capacity = *cli_options.master_document_capacity;
    }
    if(cli_options.worker_role.value.has_value()) {
        options.worker_role = cli_options.worker_role->value;
    }

    return options;
}

auto print_usage() -> void {
    deco::cli::Dispatcher<CliOptions> dispatcher("clice [OPTIONS]");
    dispatcher.usage(std::cerr, true);
}

auto run_with_options(const Options& options) -> int {
    switch(options.mode) {
        case Mode::Pipe: return clice::server::run_pipe_mode(options);
        case Mode::Socket: return clice::server::run_socket_mode(options);
        case Mode::Worker: return clice::server::run_worker_mode(options);
    }
    return 1;
}

}  // namespace

int main(int argc, const char** argv) {
    eventide::serde::config::set_field_rename_policy<eventide::serde::rename_policy::lower_camel>();
    clice::logging::stderr_logger("clice", clice::logging::options);

    auto args = deco::util::argvify(argc, argv);
    auto parsed = deco::cli::parse<CliOptions>(args);
    if(!parsed) {
        std::fprintf(stderr, "%s\n", parsed.error().message.c_str());
        print_usage();
        return 1;
    }

    const auto& cli_options = parsed->options;
    if(cli_options.help.value.value_or(false)) {
        print_usage();
        return 0;
    }

    auto options = build_options(cli_options, argc, argv);
    if(!options) {
        std::fprintf(stderr, "%s\n", options.error().c_str());
        print_usage();
        return 1;
    }

    return run_with_options(*options);
}

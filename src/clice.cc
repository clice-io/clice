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

namespace {

using clice::server::Mode;
using clice::server::Options;

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

    DECO_CFG_END();
};

auto search_in_path(std::string_view name) -> std::string {
    const char* path_env = std::getenv("PATH");
    if(!path_env || name.empty()) {
        return std::string(name);
    }

#ifdef _WIN32
    constexpr char path_sep = ';';

    // Get PATHEXT or use default extensions
    std::vector<std::string> extensions;
    const char* pathext_env = std::getenv("PATHEXT");
    if(pathext_env) {
        std::string_view pathext_view(pathext_env);
        size_t ext_start = 0;
        while(ext_start < pathext_view.size()) {
            size_t ext_end = pathext_view.find(';', ext_start);
            if(ext_end == std::string_view::npos) {
                ext_end = pathext_view.size();
            }
            std::string_view ext = pathext_view.substr(ext_start, ext_end - ext_start);
            if(!ext.empty()) {
                extensions.emplace_back(ext);
            }
            ext_start = ext_end + 1;
        }
    } else {
        extensions = {".exe", ".cmd", ".bat", ".com"};
    }

    // Check if name already has an extension
    bool has_extension = name.find('.') != std::string_view::npos;

    auto is_executable = [](const std::filesystem::path& p) {
        std::error_code ec;
        auto status = std::filesystem::status(p, ec);
        return !ec && std::filesystem::exists(status) && !std::filesystem::is_directory(status);
    };
#else
    constexpr char path_sep = ':';
    auto is_executable = [](const std::filesystem::path& p) {
        std::error_code ec;
        auto status = std::filesystem::status(p, ec);
        if(ec || !std::filesystem::exists(status) || std::filesystem::is_directory(status)) {
            return false;
        }
        return (status.permissions() & (std::filesystem::perms::owner_exec |
                                        std::filesystem::perms::group_exec |
                                        std::filesystem::perms::others_exec)) != std::filesystem::perms::none;
    };
#endif

    std::string_view path_view(path_env);
    size_t start = 0;
    while(start < path_view.size()) {
        size_t end = path_view.find(path_sep, start);
        if(end == std::string_view::npos) {
            end = path_view.size();
        }

        std::string_view dir = path_view.substr(start, end - start);
        if(!dir.empty()) {
#ifdef _WIN32
            // Try the name as-is first
            std::filesystem::path full_path = std::filesystem::path(dir) / name;
            std::error_code ec;
            auto canonical = std::filesystem::canonical(full_path, ec);
            if(!ec && is_executable(canonical)) {
                return canonical.string();
            }

            // If name doesn't have an extension, try each PATHEXT extension
            if(!has_extension) {
                for(const auto& ext : extensions) {
                    std::string name_with_ext = std::string(name) + ext;
                    full_path = std::filesystem::path(dir) / name_with_ext;
                    canonical = std::filesystem::canonical(full_path, ec);
                    if(!ec && is_executable(canonical)) {
                        return canonical.string();
                    }
                }
            }
#else
            std::filesystem::path full_path = std::filesystem::path(dir) / name;
            std::error_code ec;
            auto canonical = std::filesystem::canonical(full_path, ec);
            if(!ec && is_executable(canonical)) {
                return canonical.string();
            }
#endif
        }

        start = end + 1;
    }

    return std::string(name);
}

auto resolve_self_path(int argc, const char** argv) -> std::string {
    if(argc <= 0 || argv == nullptr || argv[0] == nullptr) {
        return "clice";
    }

    std::string_view arg0(argv[0]);
    std::error_code ec;

    // If arg0 contains a path separator, treat it as a path (relative or absolute)
    if(arg0.find('/') != std::string_view::npos || arg0.find('\\') != std::string_view::npos) {
        auto absolute = std::filesystem::absolute(arg0, ec);
        if(!ec) {
            auto canonical = std::filesystem::weakly_canonical(absolute, ec);
            if(!ec) {
                return canonical.string();
            }
        }
        return std::string(arg0);
    }

    // No path separator - search in PATH
    return search_in_path(arg0);
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

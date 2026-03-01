#pragma once

#include <cstddef>
#include <string>

namespace clice::server {

enum class Mode {
    Pipe,
    Socket,
    Worker,
};

enum class WorkerRole {
    Stateful,
    Stateless,
};

struct Options {
    Mode mode = Mode::Pipe;
    std::string host = "127.0.0.1";
    int port = 50051;

    std::string self_path;

    std::size_t worker_count = 2;
    std::size_t stateless_worker_count = 2;
    std::size_t worker_document_capacity = 32;
    std::size_t master_document_capacity = 256;
    WorkerRole worker_role = WorkerRole::Stateful;
};

auto run_pipe_mode(const Options& options) -> int;

auto run_socket_mode(const Options& options) -> int;

auto run_worker_mode(const Options& options) -> int;

}  // namespace clice::server

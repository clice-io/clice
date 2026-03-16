#pragma once

#include <cstddef>
#include <string>

namespace clice {

struct Options {
    enum class Mode { Pipe, Socket, StatelessWorker, StatefulWorker };

    Mode mode = Mode::Pipe;
    std::string host = "127.0.0.1";
    int port = 50051;
    std::string self_path;

    std::size_t stateless_worker_count = 0;
    std::size_t stateful_worker_count = 0;
    std::size_t worker_memory_limit = 0;

    static Options parse(int argc, const char** argv);

    void compute_defaults();
};

}  // namespace clice

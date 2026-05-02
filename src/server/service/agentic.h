#pragma once

#include <string>

#include "llvm/ADT/StringRef.h"

namespace clice {

struct AgenticQueryOptions {
    std::string host;
    int port = 0;
    std::string method;
    std::string path;
    std::string name;
    std::string query;
    int line = 0;
    std::string direction;
};

int run_agentic_mode(const AgenticQueryOptions& opts);

int run_relay_mode(llvm::StringRef socket_path);

}  // namespace clice

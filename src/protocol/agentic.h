#pragma once

#include <string>
#include <vector>

#include "kota/ipc/protocol.h"

namespace clice::agentic {

struct CompileCommandParams {
    std::string path;
};

struct CompileCommandResult {
    std::string file;
    std::string directory;
    std::vector<std::string> arguments;
};

}  // namespace clice::agentic

namespace kota::ipc::protocol {

template <>
struct RequestTraits<clice::agentic::CompileCommandParams> {
    using Result = clice::agentic::CompileCommandResult;
    constexpr inline static std::string_view method = "agentic/compileCommand";
};

}  // namespace kota::ipc::protocol

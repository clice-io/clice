#pragma once

/// Shared utilities for stateful and stateless worker processes.

#include <chrono>
#include <string>
#include <vector>

#include "compile/compilation.h"
#include "eventide/ipc/json_codec.h"
#include "eventide/serde/json/serializer.h"
#include "eventide/serde/serde/raw_value.h"

namespace clice {

/// RAII timer for measuring elapsed milliseconds.
struct ScopedTimer {
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();

    long long ms() const {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now() - start)
            .count();
    }
};

/// Fill CompilationParams directory and arguments from worker request fields.
inline void fill_args(CompilationParams& cp,
                      const std::string& directory,
                      const std::vector<std::string>& arguments) {
    cp.directory = directory;
    for(auto& arg: arguments) {
        cp.arguments.push_back(arg.c_str());
    }
}

/// Serialize a value to JSON RawValue using LSP config.
template <typename T>
inline eventide::serde::RawValue to_raw(const T& value) {
    auto json = eventide::serde::json::to_json<eventide::ipc::lsp_config>(value);
    return eventide::serde::RawValue{json ? std::move(*json) : "null"};
}

}  // namespace clice

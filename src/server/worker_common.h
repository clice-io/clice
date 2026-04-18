#pragma once

/// Shared utilities for stateful and stateless worker processes.

#include <chrono>
#include <string>
#include <vector>

#include "compile/compilation.h"

#include "kota/codec/json/serializer.h"
#include "kota/codec/raw_value.h"
#include "kota/ipc/codec/json.h"

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
inline kota::codec::RawValue to_raw(const T& value) {
    auto json = kota::codec::json::to_json<kota::ipc::lsp_config>(value);
    return kota::codec::RawValue{json ? std::move(*json) : "null"};
}

}  // namespace clice

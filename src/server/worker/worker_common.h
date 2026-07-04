#pragma once

/// Shared utilities for stateful and stateless worker processes.

#include <string>
#include <vector>

#include "compile/compilation.h"
#include "server/protocol/worker.h"
#include "support/timer.h"

#include "kota/codec/json/json.h"
#include "kota/ipc/codec/json.h"

namespace clice {

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

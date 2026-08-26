#pragma once

/// Shared utilities for stateful and stateless worker processes.

#include <string>
#include <vector>

#include "compile/compilation.h"
#include "support/timer.h"
#include "worker/protocol.h"
#include "worker/serialize.h"

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

}  // namespace clice

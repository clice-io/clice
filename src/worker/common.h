#pragma once

/// Shared utilities for stateful and stateless worker processes.

#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "compile/compilation.h"
#include "support/timer.h"
#include "vfs/file_system.h"
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

/// Hand a compile the master's PCH and PCMs. They come from clice's store,
/// so the process keeps their mappings for later compiles.
inline void use_artifacts(CompilationParams& cp,
                          const std::pair<std::string, std::uint32_t>& pch,
                          const std::unordered_map<std::string, std::string>& pcms) {
    if(!pch.first.empty()) {
        cp.pch = pch;
        vfs::keep_mapped(pch.first);
    }
    for(auto& [name, path]: pcms) {
        cp.pcms.try_emplace(name, path);
        vfs::keep_mapped(path);
    }
}

}  // namespace clice

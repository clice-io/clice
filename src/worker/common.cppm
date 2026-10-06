module;

#include "modules/prelude.h"

/// Shared utilities for stateful and stateless worker processes.

module clice:worker.common;

import :compile.compilation;
import :support.timer;
import :vfs.file_system;
import :worker.protocol;
import :worker.serialize;

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

/// Hand a compile the master's PCH and PCMs. The PCH comes from clice's
/// store, so the process keeps its mapping for later compiles.
inline void use_artifacts(CompilationParams& cp,
                          const std::pair<std::string, std::uint32_t>& pch,
                          const std::unordered_map<std::string, std::string>& pcms) {
    if(!pch.first.empty()) {
        cp.pch = pch;
        vfs::keep_mapped(pch.first);
    }
    for(auto& [name, path]: pcms) {
        cp.pcms.try_emplace(name, path);
    }
}

/// The largest index blob a worker reply carries. The transport refuses a
/// frame past 64 MiB (kotatsu's limit) and the whole reply is lost with it,
/// so a larger index is dropped at the source and the rest of the reply
/// still lands; the margin leaves room for that rest. Tests lower it
/// through CLICE_TEST_MAX_INDEX_BYTES.
std::size_t max_index_bytes();

}  // namespace clice

module;

#include "modules/prelude.h"

/// Shared utilities for stateful and stateless worker processes.

module clice:worker.common;

import :compile.compilation;
import :feature.feature;
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

/// The largest index blob a worker reply carries inline. The transport
/// refuses a frame past 64 MiB (kotatsu's limit) and the whole reply is
/// lost with it; the margin leaves room for the rest of the reply. Tests
/// lower it through CLICE_TEST_MAX_INDEX_BYTES.
std::size_t max_index_bytes();

/// Put a built index into `result`: inline when it fits, else written to
/// `output_path` — the master's transfer file — with the reply saying so.
/// Returns why the index cannot be handed over: too large with no path to
/// write to, or the write failed.
template <typename Result>
std::optional<std::string> hand_over_index(std::string envelope,
                                           llvm::StringRef output_path,
                                           Result& result) {
    if(envelope.size() <= max_index_bytes()) {
        result.tu_index_data = std::move(envelope);
        return std::nullopt;
    }
    if(output_path.empty()) {
        return std::format("the index ({} MiB) is too large to send between clice processes",
                           envelope.size() / (1024 * 1024));
    }
    if(auto error = vfs::write(output_path, envelope)) {
        return std::format("writing the index to {} failed: {}", output_path, error.message());
    }
    result.index_in_file = true;
    return std::nullopt;
}

/// What a compile reports in place of an index that could not be handed
/// over.
inline kota::ipc::protocol::Diagnostic index_unavailable(llvm::StringRef cause) {
    return feature::file_warning(std::format(
        "{}; features that read the file's own index, such as references within it, are "
        "unavailable",
        cause));
}

}  // namespace clice

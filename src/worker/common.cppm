/// Shared utilities for stateful and stateless worker processes.

module clice:worker.common;

import :compile.compilation;
import :support.logging;
import :support.prelude;
import :support.process;
import :support.text;
import :vfs.file_system;
import :worker.protocol;

namespace clice {

/// Hand a compile the master's PCH and PCMs. The PCH comes from clice's
/// store: the compile reads it through the mapping returned, which later
/// compiles share while it is held (vfs::keep_mapped).
inline std::shared_ptr<const llvm::MemoryBuffer>
    use_precompiled(CompilationInput& input,
                    const std::pair<std::string, std::uint32_t>& pch,
                    const std::unordered_map<std::string, std::string>& pcms) {
    for(auto& [name, path]: pcms) {
        input.pcms.try_emplace(name, path);
    }
    if(pch.first.empty()) {
        return nullptr;
    }
    input.pch = pch;
    return vfs::keep_mapped(pch.first);
}

/// Log into the directory the master names once it knows where the
/// session logs (worker::LogParams).
inline void log_where_told(ipc::BincodePeer& peer) {
    peer.on_notification([](const worker::LogParams& params) { logging::file_logger(params.dir); });
}

/// The largest index blob a worker reply carries inline. The transport
/// refuses a frame past 64 MiB (kotatsu's limit) and the whole reply is
/// lost with it; the margin leaves room for the rest of the reply. Tests
/// lower it through CLICE_TEST_MAX_INDEX_BYTES.
inline std::size_t max_index_bytes() {
    static std::size_t limit = env_integer("CLICE_TEST_MAX_INDEX_BYTES").value_or(56 * 1024 * 1024);
    return limit;
}

/// Put a built index into `result`: inline when it fits, else written to
/// `output_path` — the master's transfer file — with the reply saying so.
/// Returns why the index cannot be sent: too large with no path to
/// write to, or the write failed.
template <typename Result>
std::optional<std::string> send_index(std::vector<std::byte> blob,
                                      llvm::StringRef output_path,
                                      Result& result) {
    if(blob.size() <= max_index_bytes()) {
        result.tu_index_data = std::move(blob);
        return std::nullopt;
    }
    if(output_path.empty()) {
        return std::format("the index ({} MiB) is too large to send between clice processes",
                           blob.size() / (1024 * 1024));
    }
    if(auto error = fs::write_file(output_path, as_chars(blob))) {
        return std::format("writing the index to {} failed: {}", output_path, error.message());
    }
    result.index_in_file = true;
    return std::nullopt;
}

}  // namespace clice

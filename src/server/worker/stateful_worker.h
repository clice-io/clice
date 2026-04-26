#pragma once

#include <cstdint>
#include <string>

namespace clice {

/// Run the stateful worker process mode.
/// The worker holds compiled ASTs and handles feature requests
/// (hover, semantic tokens, etc.) alongside compile requests.
int run_stateful_worker_mode(std::uint64_t memory_limit,
                             const std::string& worker_name,
                             const std::string& log_dir);

}  // namespace clice

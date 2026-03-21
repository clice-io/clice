#pragma once

#include <cstdint>

namespace clice {

/// Run the stateful worker process mode.
/// The worker holds compiled ASTs and handles feature requests
/// (hover, semantic tokens, etc.) alongside compile requests.
int run_stateful_worker_mode(std::uint64_t memory_limit);

}  // namespace clice

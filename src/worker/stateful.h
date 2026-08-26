#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace clice {

/// Default cap on documents a stateful worker keeps compiled at once;
/// the least recently used document past it is evicted (the master learns
/// through an EvictedParams notification). Overridable per process via
/// `--max-documents`, which tests use to drive eviction cheaply.
constexpr inline std::size_t default_max_documents = 16;

/// Run the stateful worker process mode.
/// The worker holds compiled ASTs and handles feature requests
/// (hover, semantic tokens, etc.) alongside compile requests.
int run_stateful_worker_mode(std::uint64_t memory_limit,
                             const std::string& worker_name,
                             const std::string& log_dir,
                             std::size_t max_documents = default_max_documents);

}  // namespace clice

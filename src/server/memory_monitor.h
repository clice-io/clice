#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>

#include "server/worker_pool.h"

#include "eventide/async/async.h"

namespace clice {

namespace et = eventide;

/// Monitors worker process memory usage and triggers eviction
/// when limits are exceeded. Uses platform-specific APIs.
class MemoryMonitor {
public:
    static std::size_t get_process_memory(int pid);

    et::task<> run(WorkerPool& pool, et::event_loop& loop,
                   std::chrono::seconds interval = std::chrono::seconds(5));
};

}  // namespace clice

module;

#include "support/prelude.h"

module clice;

import :worker.resources;

namespace clice {

MemoryState memory_state(std::span<const sys::ProcessStats> workers) {
    auto memory = sys::memory(workers);
    if(!memory) {
        return {};
    }
    constexpr std::uint64_t MiB = 1ull << 20;
#ifdef __APPLE__
    auto reserve = std::max(1536 * MiB, memory->limit / 10);
#else
    auto reserve = std::max(512 * MiB, memory->limit / 20);
#endif
    return {.headroom = memory->available, .reserve = reserve, .limit = memory->limit};
}

}  // namespace clice

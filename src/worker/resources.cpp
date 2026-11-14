module clice;

import :worker.resources;

namespace clice {

MemoryState memory_state(std::span<const sys::ProcessStats> workers) {
    auto memory = sys::memory(workers);
    if(!memory) {
        return {};
    }
#ifdef __APPLE__
    constexpr bool macos = true;
#else
    constexpr bool macos = false;
#endif
    return {.headroom = memory->available,
            .reserve = memory_reserve(memory->limit, macos),
            .limit = memory->limit};
}

std::uint64_t memory_reserve(std::uint64_t limit, bool macos) {
    constexpr std::uint64_t MiB = 1ull << 20;
    return macos ? 1536 * MiB : std::max(512 * MiB, limit / 20);
}

std::size_t default_max_resident(std::uint64_t limit) {
    return std::clamp<std::uint64_t>(limit / 1'000'000'000, 8, 32);
}

}  // namespace clice

module clice:worker.resources;

import :support.prelude;

namespace clice {

/// The memory clice can still take before reclaim starts to hurt.
struct MemoryState {
    /// What is left: sys::Memory::available.
    std::uint64_t headroom = 0;

    /// The headroom kept free (memory_reserve).
    std::uint64_t reserve = 0;

    /// The physical memory, or the tightest limit clice runs under.
    std::uint64_t limit = 0;
};

/// The headroom kept free under `limit`: 5% of it, at least 512 MiB; on
/// macOS 1.5 GiB, as decompression starts near 20% of a 7 GB Mac, whose
/// headroom already leaves out what the compressor holds.
std::uint64_t memory_reserve(std::uint64_t limit, bool macos);

/// Read the machine's memory. macOS's figures need the workers' usage:
/// the system counts their compressed pages without telling whose.
MemoryState memory_state(std::span<const sys::ProcessStats> workers);

/// How many documents the stateful workers keep compiled under `limit`
/// (MemoryState::limit): one per GB, at least 8 and at most 32. A user who
/// wants clice to hold less closes files.
std::size_t default_max_resident(std::uint64_t limit);

}  // namespace clice

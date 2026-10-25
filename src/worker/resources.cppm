module;

#include "support/prelude.h"

module clice:worker.resources;

namespace clice {

/// The memory clice can still take before reclaim starts to hurt.
struct MemoryState {
    /// What is left: sys::Memory::available.
    std::uint64_t headroom = 0;

    /// The headroom kept free: 5% of the limit, at least 512 MiB; on macOS
    /// 10%, at least 1.5 GiB, as decompression starts near 20% of a 7 GB
    /// Mac.
    std::uint64_t reserve = 0;

    /// The physical memory, or the tightest limit clice runs under.
    std::uint64_t limit = 0;
};

/// Read the machine's memory. macOS's figures need the workers' usage:
/// the system counts their compressed pages without telling whose.
MemoryState memory_state(std::span<const sys::ProcessStats> workers);

}  // namespace clice

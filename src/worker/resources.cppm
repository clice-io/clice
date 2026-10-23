module;

#include "support/prelude.h"

module clice:worker.resources;

namespace clice {

/// What a process uses, as the worker scheduler reads it.
struct ProcessUsage {
    /// The process's own memory: anonymous on Linux, swapped out or not,
    /// its footprint on macOS (compressed pages included), private bytes on
    /// Windows. Clean file mappings are left out: the PCHs and the binary
    /// every worker maps once for all.
    std::uint64_t memory = 0;

    /// The pages in memory, file pages included.
    std::uint64_t resident = 0;

    /// Linux: the part of `memory` the kernel moved out to swap.
    std::uint64_t swapped = 0;

    std::chrono::milliseconds cpu{0};
};

/// The usage of a running process; unset when it cannot be read (it
/// exited, or this platform has no reader).
std::optional<ProcessUsage> process_usage(std::uint32_t pid);

/// The memory clice can still take before reclaim starts to hurt.
struct MemoryState {
    /// What is left: on Linux the least of MemAvailable and, at each level
    /// of the memory cgroups clice runs in, its limit less its usage plus
    /// the file cache no process maps; on macOS the physical memory less
    /// what the rest of the system holds (wired, anonymous but not
    /// reusable, and compressed) less the workers' footprints; on Windows
    /// the least of the available physical memory and the commit charge
    /// left.
    std::uint64_t headroom = 0;

    /// The headroom kept free: 5% of the limit, at least 512 MiB; on macOS
    /// 10%, at least 1.5 GiB, as decompression starts near 20% of a 7 GB
    /// Mac.
    std::uint64_t reserve = 0;

    /// The physical memory, or the tightest cgroup limit.
    std::uint64_t limit = 0;
};

/// Read the machine's memory. macOS's figures need the workers' usage:
/// the system counts their compressed pages without telling whose.
MemoryState memory_state(llvm::ArrayRef<ProcessUsage> workers);

/// The machine's memory as macOS counts it, in bytes.
struct HostMemory {
    std::uint64_t total = 0;

    std::uint64_t wired = 0;

    /// The anonymous pages in memory.
    std::uint64_t anonymous = 0;

    /// The anonymous pages processes gave back as reusable: macOS takes them
    /// without compressing them, and no footprint counts them. mimalloc,
    /// clice's allocator, unmarks a page before it uses it again; libmalloc,
    /// the rest of the system's, leaves some it reuses marked, out of the
    /// 0.5–0.7 GB a fresh 7 GB runner marks in all.
    std::uint64_t reusable = 0;

    /// The compressor's pages, and what they hold before compression.
    std::uint64_t compressor = 0;

    std::uint64_t compressed = 0;
};

/// macOS's headroom (see MemoryState), from the machine's counts and the
/// workers' usage.
std::uint64_t macos_headroom(const HostMemory& host, llvm::ArrayRef<ProcessUsage> workers);

/// The memory cgroups of a Linux process, for its /proc/<pid>/cgroup
/// text, with their hierarchy mounted at `root` (/sys/fs/cgroup): the
/// tightest limit and the least headroom of every level that has one;
/// unset when no level has a limit.
struct CgroupMemory {
    std::uint64_t limit = 0;

    std::uint64_t headroom = 0;
};

std::optional<CgroupMemory> cgroup_memory(llvm::StringRef root, llvm::StringRef proc_cgroup);

}  // namespace clice

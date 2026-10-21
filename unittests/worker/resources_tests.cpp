module;

#include "modules/prelude.h"

module clice;

import :unittests.utils;
import :unittests.utils.temp_dir;
import :worker.resources;

namespace clice::testing {

namespace {

constexpr std::uint64_t MiB = 1ull << 20;
constexpr std::uint64_t GiB = 1ull << 30;

std::string bytes(std::uint64_t value) {
    return std::to_string(value);
}

ZEST_SUITE(WorkerResources) {

ZEST_CASE(CgroupTwoTightestLevel) {
    TempDir root;
    root.touch("user.slice/memory.max", bytes(8 * GiB));
    root.touch("user.slice/memory.current", bytes(6 * GiB));
    root.touch(
        "user.slice/memory.stat",
        std::format("active_file {}\ninactive_file {}\nfile_mapped {}\n", GiB, GiB, 512 * MiB));
    root.touch("user.slice/app/memory.max", "max\n");
    root.touch("user.slice/app/memory.high", bytes(4 * GiB));
    root.touch("user.slice/app/memory.current", bytes(3 * GiB));
    root.touch("user.slice/app/memory.stat",
               std::format("active_file 0\ninactive_file {}\nfile_mapped 0\n", 512 * MiB));

    auto memory = cgroup_memory(root.path(""), "0::/user.slice/app\n");
    ZASSERT(memory.has_value());
    // The child's memory.high is the tighter limit; its headroom is the
    // limit less the usage, plus the file cache nothing maps.
    ZEXPECT(memory->limit == 4 * GiB);
    ZEXPECT(memory->headroom == 1 * GiB + 512 * MiB);
}

ZEST_CASE(CgroupTwoParentTighter) {
    TempDir root;
    root.touch("a/memory.max", bytes(2 * GiB));
    root.touch("a/memory.current", bytes(2 * GiB));
    root.touch("a/b/memory.max", bytes(16 * GiB));
    root.touch("a/b/memory.current", bytes(GiB));

    auto memory = cgroup_memory(root.path(""), "0::/a/b\n");
    ZASSERT(memory.has_value());
    ZEXPECT(memory->limit == 2 * GiB);
    ZEXPECT(memory->headroom == 0u);
}

ZEST_CASE(CgroupAboveHighLimit) {
    TempDir root;
    root.touch("a/memory.high", bytes(4 * GiB));
    root.touch("a/memory.current", bytes(6 * GiB));
    root.touch("a/memory.stat", std::format("inactive_file {}\nfile_mapped 0\n", 3 * GiB));

    // Throttled past its limit, the cgroup has the cache back only for the
    // usage above it.
    auto memory = cgroup_memory(root.path(""), "0::/a\n");
    ZASSERT(memory.has_value());
    ZEXPECT(memory->headroom == 1 * GiB);
}

ZEST_CASE(CgroupTwoUnlimited) {
    TempDir root;
    root.touch("a/memory.max", "max\n");
    root.touch("a/memory.high", "max\n");
    root.touch("a/memory.current", bytes(GiB));
    ZEXPECT(!cgroup_memory(root.path(""), "0::/a\n").has_value());
}

ZEST_CASE(CgroupOneHierarchical) {
    TempDir root;
    root.touch("memory/docker/abc/memory.usage_in_bytes", bytes(3 * GiB));
    root.touch("memory/docker/abc/memory.stat",
               std::format("hierarchical_memory_limit {}\ntotal_active_file {}\n"
                           "total_inactive_file {}\ntotal_mapped_file {}\n",
                           4 * GiB,
                           GiB,
                           GiB,
                           GiB));

    auto memory = cgroup_memory(root.path(""), "5:cpu:/x\n12:memory:/docker/abc\n");
    ZASSERT(memory.has_value());
    ZEXPECT(memory->limit == 4 * GiB);
    ZEXPECT(memory->headroom == 2 * GiB);
}

ZEST_CASE(CgroupOneJoinedControllers) {
    TempDir root;
    root.touch("memory/a/memory.usage_in_bytes", bytes(GiB));
    root.touch("memory/a/memory.stat", std::format("hierarchical_memory_limit {}\n", 2 * GiB));

    auto memory = cgroup_memory(root.path(""), "4:cpu,memory:/a\n");
    ZASSERT(memory.has_value());
    ZEXPECT(memory->limit == 2 * GiB);
}

ZEST_CASE(CgroupOneRootMount) {
    // A container without a cgroup namespace mounts its own cgroup at the
    // hierarchy's root, while /proc/self/cgroup names the path outside.
    TempDir root;
    root.touch("memory/memory.usage_in_bytes", bytes(GiB));
    root.touch("memory/memory.stat", std::format("hierarchical_memory_limit {}\n", 2 * GiB));

    auto memory = cgroup_memory(root.path(""), "12:memory:/docker/abc\n");
    ZASSERT(memory.has_value());
    ZEXPECT(memory->limit == 2 * GiB);
    ZEXPECT(memory->headroom == GiB);
}

ZEST_CASE(CgroupOneUnlimited) {
    TempDir root;
    root.touch("memory/memory.usage_in_bytes", bytes(GiB));
    root.touch("memory/memory.stat", "hierarchical_memory_limit 9223372036854771712\n");
    ZEXPECT(!cgroup_memory(root.path(""), "12:memory:/\n").has_value());
}

ZEST_CASE(MacFilePagesExcluded) {
    // A worker mapping a 1 GiB file: resident 2 GiB, 1 GiB of it
    // anonymous, which the machine's anonymous count holds.
    HostMemory host{.total = 16 * GiB, .wired = 2 * GiB, .anonymous = 5 * GiB};
    std::vector<ProcessUsage> workers{
        {.memory = 1 * GiB, .resident = 2 * GiB}
    };
    ZEXPECT(macos_headroom(host, workers) == 9 * GiB);
}

ZEST_CASE(MacReusablePagesFree) {
    // A worker whose allocator gave 1 GiB back as reusable: resident 2 GiB,
    // its footprint 1 GiB; the machine's anonymous count holds both.
    HostMemory host{.total = 16 * GiB, .wired = 2 * GiB, .anonymous = 5 * GiB, .reusable = 1 * GiB};
    std::vector<ProcessUsage> workers{
        {.memory = 1 * GiB, .resident = 2 * GiB}
    };
    ZEXPECT(macos_headroom(host, workers) == 10 * GiB);
}

ZEST_CASE(MacCompressedShare) {
    // 2 GiB of the worker's 3 compressed fourfold: the compressor holds
    // 512 MiB of it.
    HostMemory host{.total = 16 * GiB,
                    .wired = 2 * GiB,
                    .anonymous = 4 * GiB,
                    .compressor = 1 * GiB,
                    .compressed = 4 * GiB};
    std::vector<ProcessUsage> workers{
        {.memory = 3 * GiB, .resident = 1 * GiB}
    };
    ZEXPECT(macos_headroom(host, workers) == 7 * GiB + 512 * MiB);
}

ZEST_CASE(OwnProcessUsage) {
    auto usage = process_usage(static_cast<std::uint32_t>(llvm::sys::Process::getProcessId()));
    ZASSERT(usage.has_value());
    ZEXPECT(usage->memory > 0u);
    ZEXPECT(usage->resident > 0u);
}

ZEST_CASE(MachineMemory) {
    auto memory = memory_state({});
    ZEXPECT(memory.limit > 0u);
    ZEXPECT(memory.headroom <= memory.limit);
    ZEXPECT(memory.reserve >= 512 * MiB);
}

};  // ZEST_SUITE(WorkerResources)

}  // namespace

}  // namespace clice::testing

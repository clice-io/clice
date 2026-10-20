module;

#include "modules/prelude.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
// After windows.h, whose types it uses.
#include <psapi.h>
#elif defined(__APPLE__)
#include <libproc.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <sys/resource.h>
#include <sys/sysctl.h>
#else
#include <unistd.h>
#endif

module clice;

import :worker.resources;

namespace clice {

namespace {

constexpr std::uint64_t MiB = 1ull << 20;

std::optional<std::string> read_text(const llvm::Twine& path) {
    auto buffer = llvm::MemoryBuffer::getFileAsStream(path);
    if(!buffer) {
        return std::nullopt;
    }
    return (*buffer)->getBuffer().str();
}

/// The number after `key` at the start of a line of `text`.
std::optional<std::uint64_t> field(llvm::StringRef text, llvm::StringRef key) {
    llvm::SmallVector<llvm::StringRef> lines;
    text.split(lines, '\n', -1, false);
    for(auto line: lines) {
        if(line.consume_front(key)) {
            std::uint64_t value = 0;
            if(!line.trim().split(' ').first.getAsInteger(10, value)) {
                return value;
            }
        }
    }
    return std::nullopt;
}

/// A cgroup v2 limit file: a number, or "max".
std::optional<std::uint64_t> limit_file(const llvm::Twine& path) {
    auto text = read_text(path);
    std::uint64_t value = 0;
    if(!text || llvm::StringRef(*text).trim().getAsInteger(10, value)) {
        return std::nullopt;
    }
    return value;
}

/// What a cgroup can still take: its limit less its usage, plus the file
/// cache no process maps, which the kernel reclaims without a fault back.
std::uint64_t cgroup_headroom(std::uint64_t limit,
                              std::uint64_t usage,
                              std::uint64_t file,
                              std::uint64_t mapped) {
    auto reclaimable = file > mapped ? file - mapped : 0;
    return (limit > usage ? limit - usage : 0) + reclaimable;
}

}  // namespace

std::optional<CgroupMemory> cgroup_memory(llvm::StringRef root, llvm::StringRef proc_cgroup) {
    llvm::SmallVector<llvm::StringRef> lines;
    proc_cgroup.split(lines, '\n', -1, false);

    // cgroup v1: the memory controller's line, "N:memory:/path".
    for(auto line: lines) {
        auto [controller, path] = line.split(':').second.split(':');
        if(controller != "memory") {
            continue;
        }
        // A container without a cgroup namespace mounts its own cgroup at
        // the hierarchy's root.
        auto dir = (root + "/memory" + path).str();
        if(!llvm::sys::fs::exists(dir + "/memory.stat")) {
            dir = (root + "/memory").str();
        }
        auto stat = read_text(dir + "/memory.stat");
        auto usage = limit_file(dir + "/memory.usage_in_bytes");
        if(!stat || !usage) {
            return std::nullopt;
        }
        // The least limit of the cgroup and its ancestors; past every
        // machine's memory when none is set.
        auto limit = field(*stat, "hierarchical_memory_limit ");
        if(!limit || *limit >= (1ull << 62)) {
            return std::nullopt;
        }
        auto file = field(*stat, "total_active_file ").value_or(0) +
                    field(*stat, "total_inactive_file ").value_or(0);
        return CgroupMemory{
            .limit = *limit,
            .headroom = cgroup_headroom(*limit,
                                        *usage,
                                        file,
                                        field(*stat, "total_mapped_file ").value_or(0)),
        };
    }

    // cgroup v2: one line, "0::/path"; every level up to the root counts.
    for(auto line: lines) {
        if(!line.consume_front("0::")) {
            continue;
        }
        std::optional<CgroupMemory> tightest;
        auto dir = (root + line.rtrim('/')).str();
        while(true) {
            auto max = limit_file(dir + "/memory.max");
            auto high = limit_file(dir + "/memory.high");
            if(max || high) {
                auto limit = std::min(max.value_or(UINT64_MAX), high.value_or(UINT64_MAX));
                auto stat = read_text(dir + "/memory.stat").value_or("");
                auto file = field(stat, "active_file ").value_or(0) +
                            field(stat, "inactive_file ").value_or(0);
                auto headroom = cgroup_headroom(limit,
                                                limit_file(dir + "/memory.current").value_or(0),
                                                file,
                                                field(stat, "file_mapped ").value_or(0));
                if(!tightest) {
                    tightest = CgroupMemory{.limit = limit, .headroom = headroom};
                } else {
                    tightest->limit = std::min(tightest->limit, limit);
                    tightest->headroom = std::min(tightest->headroom, headroom);
                }
            }
            if(dir.size() <= root.size()) {
                break;
            }
            dir = llvm::sys::path::parent_path(dir, llvm::sys::path::Style::posix).str();
        }
        return tightest;
    }
    return std::nullopt;
}

#if defined(_WIN32)

std::optional<ProcessUsage> process_usage(std::uint32_t pid) {
    auto handle = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if(!handle) {
        return std::nullopt;
    }
    PROCESS_MEMORY_COUNTERS_EX counters{};
    FILETIME created, exited, kernel, user;
    auto read = ::GetProcessMemoryInfo(handle,
                                       reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                                       sizeof(counters)) &&
                ::GetProcessTimes(handle, &created, &exited, &kernel, &user);
    ::CloseHandle(handle);
    if(!read) {
        return std::nullopt;
    }
    auto ticks = [](FILETIME time) {
        return (std::uint64_t(time.dwHighDateTime) << 32) | time.dwLowDateTime;
    };
    // FILETIME counts 100 ns.
    return ProcessUsage{
        .memory = counters.PrivateUsage,
        .resident = counters.WorkingSetSize,
        .cpu = std::chrono::milliseconds((ticks(kernel) + ticks(user)) / 10'000),
    };
}

MemoryState memory_state(llvm::ArrayRef<ProcessUsage>) {
    MEMORYSTATUSEX status{.dwLength = sizeof(MEMORYSTATUSEX)};
    if(!::GlobalMemoryStatusEx(&status)) {
        return {};
    }
    auto limit = status.ullTotalPhys;
    return {
        .headroom = std::min(status.ullAvailPhys, status.ullAvailPageFile),
        .reserve = std::max(512 * MiB, limit / 20),
        .limit = limit,
    };
}

#elif defined(__APPLE__)

std::optional<ProcessUsage> process_usage(std::uint32_t pid) {
    rusage_info_v2 info{};
    if(::proc_pid_rusage(static_cast<int>(pid),
                         RUSAGE_INFO_V2,
                         reinterpret_cast<rusage_info_t*>(&info)) != 0) {
        return std::nullopt;
    }
    const static auto timebase = [] {
        mach_timebase_info_data_t base{};
        ::mach_timebase_info(&base);
        return base;
    }();
    // The CPU times are in mach absolute time units.
    auto nanoseconds = (info.ri_user_time + info.ri_system_time) * timebase.numer / timebase.denom;
    return ProcessUsage{
        .memory = info.ri_phys_footprint,
        .resident = info.ri_resident_size,
        .cpu = std::chrono::milliseconds(nanoseconds / 1'000'000),
    };
}

MemoryState memory_state(llvm::ArrayRef<ProcessUsage> workers) {
    std::uint64_t total = 0;
    auto size = sizeof(total);
    vm_statistics64_data_t vm{};
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    vm_size_t page = 0;
    if(::sysctlbyname("hw.memsize", &total, &size, nullptr, 0) != 0 ||
       ::host_statistics64(::mach_host_self(),
                           HOST_VM_INFO64,
                           reinterpret_cast<host_info64_t>(&vm),
                           &count) != KERN_SUCCESS ||
       ::host_page_size(::mach_host_self(), &page) != KERN_SUCCESS) {
        return {};
    }
    // Neither the free count nor the inactive one tells what the workers
    // can still take: macOS compresses their memory long before it frees
    // any. What the rest of the system holds is the wired, anonymous and
    // compressed memory less the workers' own resident pages and their
    // share of the compressor, at its current ratio.
    auto wired = std::uint64_t(vm.wire_count) * page;
    auto anonymous = std::uint64_t(vm.internal_page_count) * page;
    auto compressor = std::uint64_t(vm.compressor_page_count) * page;
    auto compressed = std::uint64_t(vm.total_uncompressed_pages_in_compressor) * page;
    auto ratio = compressor > 0 ? double(compressed) / double(compressor) : 1.0;
    std::uint64_t resident = 0;
    std::uint64_t footprint = 0;
    double shared_compressor = 0;
    for(auto& worker: workers) {
        resident += worker.resident;
        footprint += worker.memory;
        if(worker.memory > worker.resident) {
            shared_compressor += double(worker.memory - worker.resident) / ratio;
        }
    }
    auto held = wired + anonymous + compressor;
    auto ours = resident + std::uint64_t(shared_compressor);
    auto others = held > ours ? held - ours : 0;
    auto taken = others + footprint;
    return {
        .headroom = total > taken ? total - taken : 0,
        .reserve = std::max(1536 * MiB, total / 10),
        .limit = total,
    };
}

#else

std::optional<ProcessUsage> process_usage(std::uint32_t pid) {
    auto status = read_text(std::format("/proc/{}/status", pid));
    auto stat = read_text(std::format("/proc/{}/stat", pid));
    if(!status || !stat) {
        return std::nullopt;
    }
    // After the command, which may hold spaces, "state ppid ...": utime and
    // stime are the 12th and 13th fields from there, in clock ticks.
    auto fields = llvm::StringRef(*stat).rsplit(')').second;
    llvm::SmallVector<llvm::StringRef> values;
    fields.split(values, ' ', -1, false);
    std::uint64_t user = 0;
    std::uint64_t system = 0;
    if(values.size() < 13 || values[11].getAsInteger(10, user) ||
       values[12].getAsInteger(10, system)) {
        return std::nullopt;
    }
    const static auto ticks = ::sysconf(_SC_CLK_TCK);
    auto swapped = field(*status, "VmSwap:").value_or(0) * 1024;
    return ProcessUsage{
        .memory = field(*status, "RssAnon:").value_or(0) * 1024 + swapped,
        .resident = field(*status, "VmRSS:").value_or(0) * 1024,
        .swapped = swapped,
        .cpu = std::chrono::milliseconds((user + system) * 1000 / ticks),
    };
}

MemoryState memory_state(llvm::ArrayRef<ProcessUsage>) {
    auto meminfo = read_text("/proc/meminfo");
    if(!meminfo) {
        return {};
    }
    auto limit = field(*meminfo, "MemTotal:").value_or(0) * 1024;
    auto headroom = field(*meminfo, "MemAvailable:").value_or(0) * 1024;
    // Read afresh every time: the process can move between cgroups.
    if(auto proc_cgroup = read_text("/proc/self/cgroup")) {
        if(auto cgroup = cgroup_memory("/sys/fs/cgroup", *proc_cgroup)) {
            limit = std::min(limit, cgroup->limit);
            headroom = std::min(headroom, cgroup->headroom);
        }
    }
    return {
        .headroom = headroom,
        .reserve = std::max(512 * MiB, limit / 20),
        .limit = limit,
    };
}

#endif

}  // namespace clice

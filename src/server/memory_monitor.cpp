#include "server/memory_monitor.h"

#include "support/logging.h"

#include "eventide/async/io/watcher.h"

#ifdef __APPLE__
#include <libproc.h>
#include <mach/mach.h>
#elif defined(__linux__)
#include <fstream>
#include <string>
#endif

namespace clice {

namespace et = eventide;

std::size_t MemoryMonitor::get_process_memory(int pid) {
#ifdef __APPLE__
    struct mach_task_basic_info info;
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    task_t task;

    if(task_for_pid(mach_task_self(), pid, &task) != KERN_SUCCESS)
        return 0;

    if(task_info(task, MACH_TASK_BASIC_INFO, (task_info_t)&info, &count) != KERN_SUCCESS) {
        mach_port_deallocate(mach_task_self(), task);
        return 0;
    }

    mach_port_deallocate(mach_task_self(), task);
    return info.resident_size;
#elif defined(__linux__)
    std::string path = "/proc/" + std::to_string(pid) + "/statm";
    std::ifstream file(path);
    if(!file.is_open())
        return 0;

    std::size_t pages = 0;
    file >> pages; // total program size
    file >> pages; // resident set size
    return pages * 4096;
#else
    (void)pid;
    return 0;
#endif
}

et::task<> MemoryMonitor::run(WorkerPool& pool, et::event_loop& loop,
                               std::chrono::seconds interval) {
    while(true) {
        co_await et::sleep(interval, loop);

        // Memory monitoring is informational for now.
        // Full integration with WorkerPool PIDs requires
        // exposing worker PIDs, which will be done when
        // the master server wires everything together.
        LOG_DEBUG("MemoryMonitor: periodic check");
    }
}

}  // namespace clice

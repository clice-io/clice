#include "worker/common.h"

#include "support/logging.h"

#ifdef __GLIBC__
#include <malloc.h>
#endif

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Process.h"
#include "llvm/Support/raw_ostream.h"

namespace clice {

std::size_t max_index_bytes() {
    static std::size_t limit = [] {
        std::size_t bytes = 56 * 1024 * 1024;
        if(auto value = llvm::sys::Process::GetEnv("CLICE_TEST_MAX_INDEX_BYTES")) {
            llvm::StringRef(*value).getAsInteger(10, bytes);
        }
        return bytes;
    }();
    return limit;
}

void release_free_memory() {
#ifdef __GLIBC__
    malloc_trim(0);
#endif
}

void prefer_as_oom_victim() {
#ifdef __linux__
    std::error_code ec;
    llvm::raw_fd_ostream adj("/proc/self/oom_score_adj", ec, llvm::sys::fs::OF_None);
    if(ec) {
        LOG_WARN("Cannot adjust the OOM score: {}", ec.message());
        return;
    }
    adj << "1000";
    adj.close();
    if(adj.has_error()) {
        LOG_WARN("Cannot adjust the OOM score: {}", adj.error().message());
        adj.clear_error();
    }
#endif
}

}  // namespace clice

#include "vfs/file_table.h"

#include <chrono>

#include "llvm/ADT/ScopeExit.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/xxhash.h"

namespace clice {

std::optional<ObservedFile> read_file_observed(const char* path) {
    auto fd = llvm::sys::fs::openNativeFileForRead(path);
    if(!fd) {
        llvm::consumeError(fd.takeError());
        return std::nullopt;
    }
    auto close = llvm::make_scope_exit([&] { llvm::sys::fs::closeFile(*fd); });

    llvm::sys::fs::file_status before;
    bool have_before = !llvm::sys::fs::status(*fd, before);

    // Force read() instead of mmap (IsVolatile): the bytes must be a
    // snapshot taken between the two fstats — a mapped buffer would keep
    // tracking the file after the post-fstat, unpairing hash and stat.
    auto buf = llvm::MemoryBuffer::getOpenFile(*fd,
                                               path,
                                               /*FileSize=*/-1,
                                               /*RequiresNullTerminator=*/true,
                                               /*IsVolatile=*/true);
    if(!buf) {
        return std::nullopt;
    }

    ObservedFile result;
    result.content = std::move(*buf);

    llvm::sys::fs::file_status after;
    bool have_after = !llvm::sys::fs::status(*fd, after);
    result.obs.hash = llvm::xxh3_64bits(result.content->getBuffer());
    if(!have_after) {
        return result;
    }
    result.obs.size = after.getSize();
    result.obs.mtime_ns = fs::mtime_ns(after);
    result.obs.paired = have_before && before.getSize() == after.getSize() &&
                        fs::mtime_ns(before) == result.obs.mtime_ns;

    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::system_clock::now().time_since_epoch())
                      .count();
    result.obs.reliable =
        result.obs.paired && result.obs.mtime_ns <= fs::stat_baseline_before_ns(now_ms);
    return result;
}

}  // namespace clice

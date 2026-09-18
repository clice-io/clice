#include "index/writer_lock.h"

#include <format>

#include "version.h"
#include "support/filesystem.h"
#include "support/logging.h"

#include "kota/codec/json/json.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Process.h"
#include "llvm/Support/raw_ostream.h"

namespace clice::index {

namespace {

constexpr llvm::StringLiteral lock_name = "index.lock";
constexpr llvm::StringLiteral endpoint_name = "server.json";

/// The holder's pid as stamped into the lock file; empty when the stamp
/// is unreadable (a Windows holder keeps the file exclusive) or absent.
std::string stamped_holder(llvm::StringRef lock_path) {
    auto stamped = fs::read(lock_path);
    if(!stamped || stamped->empty()) {
        return {};
    }
    return std::format("pid {}", llvm::StringRef(*stamped).trim());
}

}  // namespace

std::optional<int> acquire_writer_lock(llvm::StringRef cache_dir) {
    auto lock_path = path::join(cache_dir, lock_name);
    int lock_fd = -1;
    if(auto ec = llvm::sys::fs::openFileForReadWrite(lock_path,
                                                     lock_fd,
                                                     llvm::sys::fs::CD_OpenAlways,
                                                     llvm::sys::fs::OF_None)) {
        LOG_WARN("Failed to open the index writer lock {}: {}", lock_path, ec.message());
        return std::nullopt;
    }
    if(llvm::sys::fs::tryLockFile(lock_fd)) {
        auto holder = stamped_holder(lock_path);
        LOG_WARN(
            "Another clice process{} is writing the index cache at {}; "
            "index persistence is disabled for this process",
            holder.empty() ? "" : std::format(" ({})", holder),
            cache_dir);
        llvm::sys::Process::SafelyCloseFileDescriptor(lock_fd);
        return std::nullopt;
    }
    llvm::sys::fs::resize_file(lock_fd, 0);
    llvm::raw_fd_ostream stamp(lock_fd, /*shouldClose=*/false);
    stamp.seek(0);
    stamp << llvm::sys::Process::getProcessId() << '\n';
    stamp.flush();
    // Best effort: a lock left unstamped only costs the next process the
    // pid in its message, while an errored stream aborts in its destructor.
    stamp.clear_error();
    return lock_fd;
}

void release_writer_lock(int lock_fd) {
    if(lock_fd != -1) {
        // The stamp names the holder; an unheld lock reads as nobody.
        llvm::sys::fs::resize_file(lock_fd, 0);
        llvm::sys::fs::unlockFile(lock_fd);
        llvm::sys::Process::SafelyCloseFileDescriptor(lock_fd);
    }
}

void write_endpoint(llvm::StringRef cache_dir, const ServerEndpoint& endpoint) {
    auto json = kota::codec::json::to_string(endpoint);
    if(!json) {
        return;
    }
    // Written whole, then renamed into place: a reader never sees a
    // partial record.
    auto final_path = path::join(cache_dir, endpoint_name);
    auto tmp_path = final_path + ".tmp";
    if(auto written = fs::write(tmp_path, *json); !written) {
        LOG_WARN("Failed to record the server endpoint at {}: {}",
                 final_path,
                 written.error().message());
        return;
    }
    if(auto renamed = fs::rename(tmp_path, final_path); !renamed) {
        LOG_WARN("Failed to record the server endpoint at {}: {}",
                 final_path,
                 renamed.error().message());
        llvm::sys::fs::remove(tmp_path);
    }
}

void remove_endpoint(llvm::StringRef cache_dir) {
    llvm::sys::fs::remove(path::join(cache_dir, endpoint_name));
}

WriterProbe probe_writer(llvm::StringRef cache_dir) {
    WriterProbe probe;
    auto lock_path = path::join(cache_dir, lock_name);
    int lock_fd = -1;
    if(auto ec = llvm::sys::fs::openFileForReadWrite(lock_path,
                                                     lock_fd,
                                                     llvm::sys::fs::CD_OpenAlways,
                                                     llvm::sys::fs::OF_None)) {
        // Whoever opens the library next reports the real cause.
        return probe;
    }
    if(!llvm::sys::fs::tryLockFile(lock_fd)) {
        // Swept under the lock: a server acquiring it right after the
        // release publishes a record this sweep must not take.
        remove_endpoint(cache_dir);
        llvm::sys::fs::unlockFile(lock_fd);
        llvm::sys::Process::SafelyCloseFileDescriptor(lock_fd);
        return probe;
    }
    llvm::sys::Process::SafelyCloseFileDescriptor(lock_fd);
    probe.state = WriterProbe::State::Held;
    probe.holder = stamped_holder(lock_path);

    auto record = fs::read(path::join(cache_dir, endpoint_name));
    if(!record) {
        return probe;
    }
    ServerEndpoint endpoint;
    if(auto parsed = kota::codec::json::from_string(*record, endpoint); !parsed) {
        return probe;
    }
    if(endpoint.version != clice::version) {
        probe.holder = std::format("a clice {} server (pid {})", endpoint.version, endpoint.pid);
        return probe;
    }
    probe.state = WriterProbe::State::Server;
    probe.endpoint = std::move(endpoint);
    probe.holder = std::format("pid {}", probe.endpoint.pid);
    return probe;
}

}  // namespace clice::index

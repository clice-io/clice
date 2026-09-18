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

/// The holder's pid as stamped into the lock file; nullopt when the stamp
/// is unreadable (a Windows holder keeps the file exclusive) or absent.
std::optional<std::uint32_t> stamped_pid(llvm::StringRef lock_path) {
    auto stamped = fs::read(lock_path);
    std::uint32_t pid = 0;
    if(!stamped || llvm::StringRef(*stamped).trim().getAsInteger(10, pid)) {
        return std::nullopt;
    }
    return pid;
}

std::string holder_name(std::optional<std::uint32_t> pid) {
    return pid ? std::format("pid {}", *pid) : std::string();
}

}  // namespace

std::optional<WriterLock> WriterLock::acquire(llvm::StringRef cache_dir) {
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
        auto holder = holder_name(stamped_pid(lock_path));
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
    return WriterLock(lock_fd);
}

void WriterLock::release() {
    if(fd != -1) {
        // The stamp names the holder; an unheld lock reads as nobody.
        llvm::sys::fs::resize_file(fd, 0);
        llvm::sys::fs::unlockFile(fd);
        llvm::sys::Process::SafelyCloseFileDescriptor(fd);
        fd = -1;
    }
}

void write_endpoint(llvm::StringRef cache_dir, const ServerEndpoint& endpoint) {
    auto json = kota::codec::json::to_string(endpoint);
    if(!json) {
        return;
    }
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

std::string held_writer_message(const WriterProbe& probe, llvm::StringRef cache_dir) {
    return std::format(
        "another clice process{} holds the index writer lock at {}; retry when it "
        "is done",
        probe.holder.empty() ? "" : std::format(" ({})", probe.holder),
        std::string_view(cache_dir));
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
    auto holder = stamped_pid(lock_path);
    probe.holder = holder_name(holder);

    auto record = fs::read(path::join(cache_dir, endpoint_name));
    if(!record) {
        return probe;
    }
    ServerEndpoint endpoint;
    if(auto parsed = kota::codec::json::from_string(*record, endpoint); !parsed) {
        return probe;
    }
    // A record from a server that died holding the lock survives until the
    // next holder records its own; the stamp tells them apart where it can
    // be read.
    if(holder && *holder != endpoint.pid) {
        return probe;
    }
    if(endpoint.version != clice::version) {
        probe.holder = std::format("a clice {} server (pid {})", endpoint.version, endpoint.pid);
        return probe;
    }
    probe.state = WriterProbe::State::Server;
    probe.endpoint = std::move(endpoint);
    return probe;
}

}  // namespace clice::index

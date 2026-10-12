module;

#include "modules/prelude.h"

#ifdef _WIN32
// See cache_store.cpp: windows.h must not spill min/max macros.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

#include "version.h"
#include "support/logging.macros.h"

module clice;

import :index.writer_lock;
import :support.logging;
import :vfs.file_system;
import :vfs.path;

namespace clice::index {

namespace {

constexpr llvm::StringLiteral lock_name = "index.lock";
constexpr llvm::StringLiteral endpoint_name = "server.json";

#ifdef _WIN32
/// The byte a writer locks, far past the pid the file records: a Windows
/// lock keeps every other process from reading the bytes it covers, and
/// LLVM's covers the whole file.
OVERLAPPED writer_byte() {
    OVERLAPPED at = {};
    at.OffsetHigh = 1;
    return at;
}
#endif

/// True when this process now holds the lock.
bool try_lock(int fd) {
#ifdef _WIN32
    auto at = writer_byte();
    return ::LockFileEx(llvm::sys::fs::convertFDToNativeFile(fd),
                        LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY,
                        0,
                        1,
                        0,
                        &at);
#else
    return !llvm::sys::fs::tryLockFile(fd);
#endif
}

void unlock(int fd) {
#ifdef _WIN32
    auto at = writer_byte();
    ::UnlockFileEx(llvm::sys::fs::convertFDToNativeFile(fd), 0, 1, 0, &at);
#else
    llvm::sys::fs::unlockFile(fd);
#endif
}

/// The holder's pid as recorded in the lock file; nullopt when absent.
std::optional<std::uint32_t> recorded_pid(llvm::StringRef lock_path) {
    auto content = vfs::read(lock_path, vfs::Read::Bytes);
    std::uint32_t pid = 0;
    if(!content || (*content)->getBuffer().trim().getAsInteger(10, pid)) {
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
    if(!try_lock(lock_fd)) {
        auto holder = holder_name(recorded_pid(lock_path));
        LOG_WARN(
            "Another clice process{} is writing the index cache at {}; "
            "index persistence is disabled for this process",
            holder.empty() ? "" : std::format(" ({})", holder),
            cache_dir);
        llvm::sys::Process::SafelyCloseFileDescriptor(lock_fd);
        return std::nullopt;
    }
    // A server that died holding the lock left its record behind; the
    // holder is now this process, which publishes its own if it serves.
    remove_endpoint(cache_dir);
    llvm::sys::fs::resize_file(lock_fd, 0);
    llvm::raw_fd_ostream out(lock_fd, /*shouldClose=*/false);
    out.seek(0);
    out << llvm::sys::Process::getProcessId() << '\n';
    out.flush();
    // Best effort: a lock left without a pid only costs the next process the
    // pid in its message, while an errored stream aborts in its destructor.
    out.clear_error();
    return WriterLock(lock_fd);
}

void WriterLock::release() {
    if(fd != -1) {
        // The recorded pid names the holder; an unheld lock reads as nobody.
        llvm::sys::fs::resize_file(fd, 0);
        unlock(fd);
        llvm::sys::Process::SafelyCloseFileDescriptor(fd);
        fd = -1;
    }
}

bool write_endpoint(llvm::StringRef cache_dir, const ServerEndpoint& endpoint) {
    auto json = kota::codec::json::to_string(endpoint);
    if(!json) {
        LOG_WARN("Failed to serialize the server endpoint record: {}", json.error().to_string());
        return false;
    }
    auto record = path::join(cache_dir, endpoint_name);
    if(auto error = vfs::write_atomic(record, *json)) {
        LOG_WARN("Failed to record the server endpoint at {}: {}", record, error.message());
        return false;
    }
    return true;
}

void remove_endpoint(llvm::StringRef cache_dir) {
    vfs::remove(path::join(cache_dir, endpoint_name));
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
    if(try_lock(lock_fd)) {
        // Removed under the lock: a server acquiring it right after the
        // release publishes a record this removal must not take.
        remove_endpoint(cache_dir);
        unlock(lock_fd);
        llvm::sys::Process::SafelyCloseFileDescriptor(lock_fd);
        return probe;
    }
    llvm::sys::Process::SafelyCloseFileDescriptor(lock_fd);
    probe.state = WriterProbe::State::Held;
    auto holder = recorded_pid(lock_path);
    probe.holder = holder_name(holder);

    auto record = vfs::read(path::join(cache_dir, endpoint_name), vfs::Read::Bytes);
    if(!record) {
        return probe;
    }
    ServerEndpoint endpoint;
    if(auto parsed = kota::codec::json::from_string((*record)->getBuffer(), endpoint); !parsed) {
        return probe;
    }
    // A record from a server that died holding the lock survives until the
    // next holder records its own; the recorded pid tells them apart.
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

module;

#include "support/prelude.h"

#include "version.h"
#include "support/logging.macros.h"

module clice;

import :index.writer_lock;
import :support.logging;
import :vfs.path;

namespace clice::index {

namespace {

constexpr llvm::StringLiteral lock_name = "index.lock";
constexpr llvm::StringLiteral endpoint_name = "server.json";

/// The holder's pid as recorded in the lock file; nullopt when absent.
std::optional<std::uint32_t> recorded_pid(llvm::StringRef lock_path) {
    std::string content;
    std::uint32_t pid = 0;
    if(fs::read_file(content, lock_path) || llvm::StringRef(content).trim().getAsInteger(10, pid)) {
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
    auto opened = fs::File::open(lock_path, {.read = true, .write = true, .create = true});
    if(!opened) {
        LOG_WARN("Failed to open the index writer lock {}: {}",
                 lock_path,
                 opened.error().message());
        return std::nullopt;
    }
    if(auto taken = opened->try_lock(); !taken || !*taken) {
        auto holder = holder_name(recorded_pid(lock_path));
        LOG_WARN(
            "Another clice process{} is writing the index cache at {}; "
            "index persistence is disabled for this process",
            holder.empty() ? "" : std::format(" ({})", holder),
            cache_dir);
        return std::nullopt;
    }
    // A server that died holding the lock left its record behind; the
    // holder is now this process, which publishes its own if it serves.
    remove_endpoint(cache_dir);
    // Best effort: a lock left without a pid only costs the next process the
    // pid in its message.
    opened->resize(0);
    opened->write(std::format("{}\n", sys::pid()));
    return WriterLock(std::move(*opened));
}

void WriterLock::release() {
    if(file.is_open()) {
        // The recorded pid names the holder; an unheld lock reads as nobody.
        file.resize(0);
        file.unlock();
        file.close();
    }
}

bool write_endpoint(llvm::StringRef cache_dir, const ServerEndpoint& endpoint) {
    auto json = json::to_string(endpoint);
    if(!json) {
        LOG_WARN("Failed to serialize the server endpoint record: {}", json.error().to_string());
        return false;
    }
    auto record = path::join(cache_dir, endpoint_name);
    if(auto error = fs::write_file_atomic(record, *json)) {
        LOG_WARN("Failed to record the server endpoint at {}: {}", record, error.message());
        return false;
    }
    return true;
}

void remove_endpoint(llvm::StringRef cache_dir) {
    fs::remove(path::join(cache_dir, endpoint_name));
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
    auto opened = fs::File::open(lock_path, {.read = true, .write = true, .create = true});
    if(!opened) {
        // Whoever opens the database next reports the real cause.
        return probe;
    }
    if(auto taken = opened->try_lock(); taken && *taken) {
        // Removed under the lock: a server acquiring it right after the
        // release publishes a record this removal must not take.
        remove_endpoint(cache_dir);
        return probe;
    }
    probe.state = WriterProbe::State::Held;
    auto holder = recorded_pid(lock_path);
    probe.holder = holder_name(holder);

    std::string record;
    if(fs::read_file(record, path::join(cache_dir, endpoint_name))) {
        return probe;
    }
    ServerEndpoint endpoint;
    if(auto parsed = json::from_string(record, endpoint); !parsed) {
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

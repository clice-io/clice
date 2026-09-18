#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "llvm/ADT/StringRef.h"

namespace clice::index {

/// One writer per cache directory. The index's global/manifest lineage
/// tolerates no second writer, and every build configuration's library
/// lives under the same cache directory, so the lock sits at its root:
/// the LSP server of the workspace holds it for its lifetime, else a
/// batch command (`clice index`, `clice lint --index`) for the run. An OS
/// advisory lock dies with its process, so a crash leaves nothing stale
/// behind. Held by the workspace that opened the store, not by the
/// database handle: a database condemned and reopened mid-session keeps
/// the directory.
class WriterLock {
public:
    /// nullopt when another process holds the lock (logged with the
    /// holder's pid where the platform lets it be read).
    static std::optional<WriterLock> acquire(llvm::StringRef cache_dir);

    WriterLock(WriterLock&& other) noexcept : fd(other.fd) {
        other.fd = -1;
    }

    WriterLock& operator=(WriterLock&& other) noexcept {
        release();
        fd = other.fd;
        other.fd = -1;
        return *this;
    }

    ~WriterLock() {
        release();
    }

private:
    explicit WriterLock(int fd) : fd(fd) {}

    void release();

    int fd;
};

/// What a serving writer records next to the lock, for the commands that
/// find the lock taken: where its control channel listens. Written only
/// by the process holding the lock, so a record found while the lock is
/// free is a crash's residue.
struct ServerEndpoint {
    std::uint32_t pid = 0;
    std::string version;
    std::string host;
    int port = 0;
};

/// False when the record could not be written (logged).
bool write_endpoint(llvm::StringRef cache_dir, const ServerEndpoint& endpoint);

void remove_endpoint(llvm::StringRef cache_dir);

/// A command's view of the writer before it decides how the index gets
/// updated: become the writer itself, ask the serving one, or give up.
struct WriterProbe {
    enum class State : std::uint8_t {
        /// Nobody holds the lock.
        Free,
        /// A clice server holds it and can be asked through `endpoint`.
        Server,
        /// Another process holds it and cannot be asked: a batch run, or
        /// a server whose record is missing or of another version.
        Held,
    };

    State state = State::Free;
    ServerEndpoint endpoint;

    /// Who holds the lock, for the user; empty when unknown.
    std::string holder;
};

/// Probe the lock without keeping it: a momentary observation — the
/// caller's own acquisition is the real gate. Never from a process that
/// holds the lock: POSIX record locks are per process, and closing the
/// probe's descriptor would release it.
WriterProbe probe_writer(llvm::StringRef cache_dir);

/// What a command tells the user when the probe found a writer it cannot
/// ask.
std::string held_writer_message(const WriterProbe& probe, llvm::StringRef cache_dir);

}  // namespace clice::index

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
/// behind. Returns the lock's descriptor, or nullopt when another process
/// holds it (logged with the holder's pid where the platform lets it be
/// read).
std::optional<int> acquire_writer_lock(llvm::StringRef cache_dir);

void release_writer_lock(int lock_fd);

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

void write_endpoint(llvm::StringRef cache_dir, const ServerEndpoint& endpoint);

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
/// caller's own acquisition is the real gate.
WriterProbe probe_writer(llvm::StringRef cache_dir);

}  // namespace clice::index

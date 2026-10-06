module;

#include "modules/prelude.h"

module clice:server.session_store;

import :server.session;
import :vfs.file_table;

namespace clice {

namespace protocol = kota::ipc::protocol;

/// The table of open documents plus the buffer-synchronization logic: the
/// single owner of editor buffer truth. Every didOpen/didChange edit lands
/// here, and every reader of an open file's text goes through the sessions
/// this store hands out.
///
/// Buffer desync (client and server drifting out of sync) is tolerated: an
/// incremental edit whose range does not fit the buffer is clamped to the
/// document per LSP 3.17 (logged at info), and requests keep being served.
/// Non-monotonic document versions are warned about at the transport edge,
/// where the protocol context lives.
///
/// Future work: this store does not yet bound the number of concurrently
/// open sessions.
struct SessionStore {
    llvm::DenseMap<Fid, std::shared_ptr<Session>> sessions;

    /// The crash records of closed documents, with the buffer hash they
    /// closed on: a reopen keeps its bars (see Quarantine) — closing a
    /// document is no retry, and a document reopened on other bytes counts
    /// the difference as a change.
    struct Parked {
        std::shared_ptr<Quarantine> quarantine;
        std::uint64_t hash = 0;
    };

    llvm::DenseMap<Fid, Parked> parked;

    /// Look up the open Session for a path_id, or nullptr if none.
    std::shared_ptr<Session> find(Fid path_id) const;

    /// Open a fresh Session for a path_id. If one already exists its
    /// generation is bumped (superseding any in-flight compile) before it is
    /// replaced; its crash records carry over.
    std::shared_ptr<Session> open(Fid path_id);

    /// Drop the Session for a path_id, bumping its generation first so a
    /// late-arriving compile result cannot resurrect stale state. Its crash
    /// records are parked for a reopen.
    void close(Fid path_id);

    /// Close the session for publishing and park its crash records ahead
    /// of the close.
    void park(Session& session);

    /// Visit every open Session. The callback returns false to stop early.
    void for_each(llvm::function_ref<bool(Fid, const Session&)> visitor) const;

    /// Apply a didOpen: install the initial buffer text, version and line
    /// starts, and bump the generation. Restores the crash records parked
    /// at close.
    void apply_open(Session& session, std::string text, int version);

    /// Apply a didChange: fold the content changes into the buffer (range →
    /// offset mapping, in-place text replacement, line-start rebuild), then
    /// bump version and generation. Ranges that do not fit the buffer are
    /// clamped per LSP 3.17 (see the struct comment on desync tolerance).
    /// The projection-side supersede (ASTFamily::supersede) is the
    /// caller's job — the store owns buffer truth only.
    void apply_change(Session& session,
                      llvm::ArrayRef<protocol::TextDocumentContentChangeEvent> changes,
                      int version);
};

}  // namespace clice

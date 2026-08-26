#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "server/state/session.h"

#include "kota/ipc/lsp/protocol.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/STLFunctionalExtras.h"

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
    llvm::DenseMap<std::uint32_t, std::shared_ptr<Session>> sessions;

    /// Look up the open Session for a path_id, or nullptr if none.
    std::shared_ptr<Session> find(std::uint32_t path_id) const;

    /// Open a fresh Session for a path_id. If one already exists its
    /// generation is bumped (superseding any in-flight compile) before it is
    /// replaced.
    std::shared_ptr<Session> open(std::uint32_t path_id);

    /// Drop the Session for a path_id, bumping its generation first so a
    /// late-arriving compile result cannot resurrect stale state.
    void close(std::uint32_t path_id);

    /// Visit every open Session. The callback returns false to stop early.
    void for_each(llvm::function_ref<bool(std::uint32_t, const Session&)> visitor) const;

    /// Apply a didOpen: install the initial buffer text, version and line
    /// starts, and bump the generation.
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

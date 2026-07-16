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

/// How much of a session's compile state a reset invalidates. Both depths
/// mark the AST dirty; they differ in which staleness token they bump.
enum class ResetDepth : std::uint8_t {
    /// The buffer's identity changed for compilation purposes (context
    /// switch, orphaned context choice): in-flight compile results no
    /// longer describe this document. Bumps generation, drops the PCH
    /// reference and dependency snapshot earned under the old identity,
    /// and re-arms the self-containment trial.
    Superseded,
    /// The built AST is gone (worker eviction/crash) or its inputs
    /// changed, but the buffer is still the same buffer. Bumps
    /// dirty_epoch so an in-flight compile cannot declare its product
    /// fresh; master-side caches (PCH, deps snapshot) stay — pull-side
    /// validation decides their fate.
    Lost,
};

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
    /// bump version, generation and mark the AST dirty. Ranges that do not
    /// fit the buffer are clamped per LSP 3.17 (see the struct comment on
    /// desync tolerance).
    void apply_change(Session& session,
                      llvm::ArrayRef<protocol::TextDocumentContentChangeEvent> changes,
                      int version);

    /// Invalidate a session's compile state to the given depth (see
    /// ResetDepth). The single reset vocabulary for every "this session
    /// must recompile" site — context switches, orphaned choices, event
    /// dispatch effects — so the token discipline lives in one place.
    /// Static so call sites that hold a Session* but no store reference
    /// (ContextResolver::switch_context) can use it; it lives here rather
    /// than on Session because state mutation vocabulary is this store's
    /// charter.
    static void reset_compile_state(Session& session, ResetDepth depth);
};

}  // namespace clice

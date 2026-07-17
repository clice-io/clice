#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "command/command.h"
#include "server/state/session.h"
#include "server/state/workspace.h"
#include "server/worker/worker_pool.h"
#include "support/signal.h"

#include "kota/async/async.h"
#include "kota/codec/json/json.h"
#include "kota/ipc/codec/json.h"
#include "kota/ipc/lsp/protocol.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"

namespace clice {

namespace testing {

struct CompilerFixture;

}

namespace protocol = kota::ipc::protocol;

class ContextResolver;

/// Convert a file:// URI to a local file path.
std::string uri_to_path(const std::string& uri);

/// Compilation service — drives worker processes to build ASTs, PCHs, and PCMs.
///
/// Compiler holds no persistent state of its own.  All project-wide data
/// lives in Workspace; per-file data lives in Session.  Compiler reads from
/// both and writes compilation results back to Session (file_index, pch_key,
/// ast_deps, diagnostics).
///
/// Responsibilities:
///   - AST compilation lifecycle (ensure_compiled → ensure_pch → ensure_deps)
///   - Feature request forwarding to stateful/stateless workers
///   - Compile argument resolution (CDB lookup + header context fallback)
///   - Compile graph initialization (module DAG setup)
///
/// NOT responsible for:
///   - Document lifecycle (didOpen/didChange/didClose) — handled by MasterServer
///   - Index queries — handled by IndexQuery
///   - Background indexing scheduling — handled by Indexer
class Compiler {
public:
    Compiler(kota::event_loop& loop,
             Workspace& workspace,
             ContextResolver& contexts,
             WorkerPool& pool);

    ~Compiler();

    void init_compile_graph();

    /// Compile an open file's AST if dirty.  On success, updates session's
    /// file_index, pch_key, ast_deps, and publishes diagnostics.
    kota::task<bool> ensure_compiled(std::shared_ptr<Session> session);

    /// Interrupt the in-flight compile if the buffer moved past it
    /// (generation mismatch): the worker abandons the stale parse at the
    /// next declaration, while the request still runs to its reply — crash
    /// accounting keeps observing the real outcome. Deliberately does NOT
    /// touch deps_scope: the supersede point orders that cancel after the
    /// replacement spawn so module interest never dips to zero across the
    /// swap. A no-op when nothing is in flight or the round is current.
    void interrupt_superseded(Session& session);

    /// The edit path's whole supersede: interrupt the worker's parse AND
    /// cancel the stale round's dependency waits. With no replacement
    /// round coming there is no interest hand-off to order against, and a
    /// round parked in dependency prep (module graph waits) would
    /// otherwise hold its waiters until the graph settles. A no-op when
    /// nothing is in flight or the round is current.
    void abandon_superseded(Session& session);

    using RawResult = kota::task<kota::codec::RawValue, kota::ipc::Error>;

    /// Forward a query to the stateful worker that holds this file's AST.
    /// Ensures compilation first.  For position-sensitive queries (hover,
    /// goto-definition), pass a Position.  For range-sensitive queries
    /// (inlay hints), pass a Range.
    /// `token`, on every forward: the LSP request's cancellation token.
    /// Passing it into the worker send turns a client $/cancelRequest into
    /// a wire cancel — the worker stops the parse at the next top-level
    /// declaration instead of computing a result nobody will read. The
    /// shared compile a query waits on is deliberately NOT cancelled: it
    /// serves every waiter, not this request.
    RawResult forward_query(worker::QueryKind kind,
                            std::shared_ptr<Session> session,
                            std::optional<protocol::Position> position = {},
                            std::optional<protocol::Range> range = {},
                            std::optional<kota::cancellation_token> token = {});

    /// Forward a build request (signature help, etc.) to a stateless worker.
    /// Sends the full buffer content and compile arguments. `token`: see
    /// forward_query.
    RawResult forward_build(worker::BuildKind kind,
                            const protocol::Position& position,
                            std::shared_ptr<Session> session,
                            std::optional<kota::cancellation_token> token = {});

    /// Forward a document-link query to the stateful worker holding this
    /// file's AST. Covers the main-file region only: the preamble's links
    /// live in the PCH's PreambleState blob (see PCHState::load_state).
    /// `token`: see forward_query.
    kota::task<std::vector<feature::DocumentLink>, kota::ipc::Error>
        forward_document_links(std::shared_ptr<Session> session,
                               std::optional<kota::cancellation_token> token = {});

    /// Forward a formatting request to a stateless worker. `token`: see
    /// forward_query.
    RawResult forward_format(std::shared_ptr<Session> session,
                             std::optional<protocol::Range> range = {},
                             std::optional<kota::cancellation_token> token = {});

    /// Emitted after a compile round materializes its publishable products
    /// into the session's `output` field (both success and the failure/
    /// clear path). Subscribers read the output from the session; with no
    /// subscriber connected the output simply stays there.
    Signal<std::shared_ptr<Session>> on_output;

    /// Callback invoked when indexing should be scheduled.
    std::function<void()> on_indexing_needed;

    /// Invoked from ensure_compiled's fast path when the pull-side
    /// staleness check finds a dependency changed on disk. The owner routes
    /// it into the event pipeline as a DiskChanged (synchronously), so lazy
    /// detection and the file tracker's polling share one invalidation
    /// cascade instead of maintaining two.
    std::function<void(std::uint32_t path_id)> on_stale;

    /// Cancel in-flight compile tasks and wait for them to finish.
    kota::task<> stop();

private:
    kota::task<> run_compile(std::shared_ptr<Session> session);

    /// Publish the quarantine diagnostic as the session's current output
    /// and mark the spell announced. `source` falls back to the previous
    /// output's command source when the announcement has no compile of its
    /// own (the ensure_compiled entry gate).
    void publish_quarantined(const std::shared_ptr<Session>& session,
                             std::optional<CommandSource> source,
                             std::optional<std::uint32_t> line_limit);

    /// Clear the published quarantine diagnostic after a stateless or
    /// query recovery lifted the quarantine: no compile ran to overwrite
    /// the output, and the stale "file is quarantined" must not linger.
    void publish_recovered(const std::shared_ptr<Session>& session);

    /// @param launch_generation, launch_epoch  The caller's staleness-token
    ///               snapshots from the moment its round took off, NOT ones
    ///               taken on entry: a round invalidated during the
    ///               dependency phase would otherwise re-snapshot the new
    ///               values here and slip a stale pch_key past the write
    ///               guards. Both tokens are needed — a supersede bumps
    ///               generation, but a Lost-type invalidation (disk or CDB
    ///               change behind an in-flight round) bumps only
    ///               dirty_epoch, and a round that resolved its command
    ///               before the event must not write pch_key back either.
    /// @param scope  When set, cancels the module-dependency wait if this
    ///               compile round is superseded by a newer one.
    kota::task<bool> ensure_deps(Session& session,
                                 std::uint64_t launch_generation,
                                 std::uint64_t launch_epoch,
                                 const std::string& directory,
                                 const std::vector<std::string>& arguments,
                                 std::pair<std::string, uint32_t>& pch,
                                 std::unordered_map<std::string, std::string>& pcms,
                                 std::optional<kota::cancellation_token> scope = {});

    kota::task<bool> ensure_pch(Session& session,
                                std::uint64_t launch_generation,
                                std::uint64_t launch_epoch,
                                const std::string& directory,
                                const std::vector<std::string>& arguments);

    /// Non-const: a passing staleness check may repair the snapshots'
    /// stat fast paths in place (see deps_changed).
    bool is_stale(Session& session);
    void record_deps(Session& session, llvm::ArrayRef<DepFile> deps, std::int64_t build_at);

    /// Retract a PCH pair the frontend could not consume: remove both
    /// blobs from the store and drop the settled cache entry, so the next
    /// ensure_pch misses and rebuilds instead of trusting corrupt bytes
    /// for the life of the store.
    void invalidate_pch(llvm::StringRef pch_key);

    kota::event_loop& loop;
    Workspace& workspace;
    ContextResolver& contexts;
    WorkerPool& pool;
    kota::task_group<> compile_tasks{loop};

    friend struct testing::CompilerFixture;
};

}  // namespace clice

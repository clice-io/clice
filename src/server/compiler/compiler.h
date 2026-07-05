#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "command/command.h"
#include "server/session/session.h"
#include "server/worker/worker_pool.h"
#include "server/workspace/workspace.h"
#include "support/signal.h"

#include "kota/async/async.h"
#include "kota/codec/json/json.h"
#include "kota/ipc/codec/json.h"
#include "kota/ipc/lsp/protocol.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"

namespace clice {

namespace protocol = kota::ipc::protocol;

class ContextResolver;

/// Convert a file:// URI to a local file path.
std::string uri_to_path(const std::string& uri);

/// Compilation service — drives worker processes to build ASTs, PCHs, and PCMs.
///
/// Compiler holds no persistent state of its own.  All project-wide data
/// lives in Workspace; per-file data lives in Session.  Compiler reads from
/// both and writes compilation results back to Session (file_index, pch_ref,
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
///   - Background indexing scheduling — handled by BackgroundIndexer
class Compiler {
public:
    Compiler(kota::event_loop& loop,
             Workspace& workspace,
             ContextResolver& contexts,
             WorkerPool& pool);

    ~Compiler();

    void init_compile_graph();

    /// Compile an open file's AST if dirty.  On success, updates session's
    /// file_index, pch_ref, ast_deps, and publishes diagnostics.
    kota::task<bool> ensure_compiled(std::shared_ptr<Session> session);

    using RawResult = kota::task<kota::codec::RawValue, kota::ipc::Error>;

    /// Forward a query to the stateful worker that holds this file's AST.
    /// Ensures compilation first.  For position-sensitive queries (hover,
    /// goto-definition), pass a Position.  For range-sensitive queries
    /// (inlay hints), pass a Range.
    RawResult forward_query(worker::QueryKind kind,
                            std::shared_ptr<Session> session,
                            std::optional<protocol::Position> position = {},
                            std::optional<protocol::Range> range = {});

    /// Forward a build request (signature help, etc.) to a stateless worker.
    /// Sends the full buffer content and compile arguments.
    RawResult forward_build(worker::BuildKind kind,
                            const protocol::Position& position,
                            std::shared_ptr<Session> session);

    /// Forward a document-link query to the stateful worker holding this
    /// file's AST. Covers the main-file region only: the preamble's links
    /// are cached master-side with the PCH (see PCHState::preamble_links).
    kota::task<std::vector<feature::DocumentLink>, kota::ipc::Error>
        forward_document_links(std::shared_ptr<Session> session);

    /// Forward a formatting request to a stateless worker.
    RawResult forward_format(std::shared_ptr<Session> session,
                             std::optional<protocol::Range> range = {});

    /// Emitted after a compile round materializes its publishable products
    /// into the session's `output` field (both success and the failure/
    /// clear path). Subscribers read the output from the session; with no
    /// subscriber connected the output simply stays there.
    Signal<std::shared_ptr<Session>> on_output;

    /// Callback invoked when indexing should be scheduled.
    std::function<void()> on_indexing_needed;

    /// Cancel in-flight compile tasks and wait for them to finish.
    kota::task<> stop();

private:
    kota::task<> run_compile(std::shared_ptr<Session> session);

    /// @param scope  When set, cancels the module-dependency wait if this
    ///               compile round is superseded by a newer one.
    kota::task<bool> ensure_deps(Session& session,
                                 const std::string& directory,
                                 const std::vector<std::string>& arguments,
                                 std::pair<std::string, uint32_t>& pch,
                                 std::unordered_map<std::string, std::string>& pcms,
                                 std::optional<kota::cancellation_token> scope = {});

    kota::task<bool> ensure_pch(Session& session,
                                const std::string& directory,
                                const std::vector<std::string>& arguments);

    bool is_stale(const Session& session);
    void record_deps(Session& session, llvm::ArrayRef<std::string> deps);

    kota::event_loop& loop;
    Workspace& workspace;
    ContextResolver& contexts;
    WorkerPool& pool;
    kota::task_group<> compile_tasks{loop};
};

}  // namespace clice

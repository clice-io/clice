#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "command/command.h"
#include "eventide/async/async.h"
#include "eventide/ipc/lsp/protocol.h"
#include "eventide/ipc/peer.h"
#include "eventide/serde/serde/raw_value.h"
#include "server/session.h"
#include "server/worker_pool.h"
#include "server/workspace.h"
#include "syntax/completion.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"

namespace clice {

namespace et = eventide;
namespace protocol = et::ipc::protocol;

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
///   - Index queries — handled by Indexer
///   - Background indexing scheduling — handled by Indexer
class Compiler {
public:
    Compiler(et::event_loop& loop,
             et::ipc::JsonPeer& peer,
             Workspace& workspace,
             WorkerPool& pool,
             llvm::DenseMap<std::uint32_t, Session>& sessions);
    ~Compiler();

    void init_compile_graph();

    /// Resolve a file's compilation command (CDB lookup + header context fallback).
    /// Returns a CompileCommand with resolved flags separated from source file identity.
    /// @param session  If non-null, used for header context resolution on open files.
    std::optional<CompileCommand> resolve_compile_command(llvm::StringRef path,
                                                          Session* session = nullptr);

    /// Compile an open file's AST if dirty.  On success, updates session's
    /// file_index, pch_ref, ast_deps, and publishes diagnostics.
    et::task<bool> ensure_compiled(Session& session);

    using RawResult = et::task<et::serde::RawValue, et::ipc::Error>;

    /// Forward a query to the stateful worker that holds this file's AST.
    /// Ensures compilation first.  For position-sensitive queries (hover,
    /// goto-definition), pass a Position.  For range-sensitive queries
    /// (inlay hints), pass a Range.
    RawResult forward_query(worker::QueryKind kind,
                            Session& session,
                            std::optional<protocol::Position> position = {},
                            std::optional<protocol::Range> range = {});

    /// Forward a build request (signature help, etc.) to a stateless worker.
    /// Sends the full buffer content and compile arguments.
    RawResult forward_build(worker::BuildKind kind,
                            const protocol::Position& position,
                            Session& session);

    /// Handle completion requests.  Detects preamble context (include/import)
    /// and serves those locally; delegates code completion to a stateless worker.
    RawResult handle_completion(const protocol::Position& position, Session& session);

    /// Send an empty diagnostics notification to clear stale markers in the editor.
    void clear_diagnostics(const std::string& uri);

    /// Callback invoked when indexing should be scheduled.
    std::function<void()> on_indexing_needed;

private:
    et::task<bool> ensure_deps(Session& session,
                               const CompileCommand& cmd,
                               std::pair<std::string, uint32_t>& pch,
                               std::unordered_map<std::string, std::string>& pcms);

    et::task<bool> ensure_pch(Session& session, const CompileCommand& cmd);

    bool is_stale(const Session& session);
    void record_deps(Session& session, llvm::ArrayRef<std::string> deps);

    void publish_diagnostics(const std::string& uri, int version, const et::serde::RawValue& diags);

    std::optional<HeaderFileContext> resolve_header_context(std::uint32_t header_path_id,
                                                            Session* session);

    std::optional<CompileCommand> resolve_header_context_command(llvm::StringRef path,
                                                                 std::uint32_t path_id,
                                                                 Session* session);

private:
    et::event_loop& loop;
    et::ipc::JsonPeer& peer;
    Workspace& workspace;
    WorkerPool& pool;
    llvm::DenseMap<std::uint32_t, Session>& sessions;
};

}  // namespace clice

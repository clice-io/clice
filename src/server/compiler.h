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

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"

namespace clice {

namespace et = eventide;
namespace protocol = et::ipc::protocol;

enum class CompletionContext { None, IncludeQuoted, IncludeAngled, Import };

struct PreambleCompletionContext {
    CompletionContext kind = CompletionContext::None;
    std::string prefix;
};

/// Convert a file:// URI to a local file path.
std::string uri_to_path(const std::string& uri);

/// Stateless compilation service.
///
/// Compiler does not own any persistent state.  It holds references to
/// Workspace (project-wide shared state) and WorkerPool (child processes),
/// both owned by MasterServer.  Per-file state lives in Session objects
/// managed by the caller.
///
/// Document lifecycle (open/change/close) is handled directly by
/// MasterServer on the sessions map.  Compiler is only responsible for:
///   - Compilation (ensure_compiled, ensure_pch, ensure_deps)
///   - Feature request forwarding to workers
///   - Compile argument resolution
///   - Compile graph initialization
class Compiler {
public:
    Compiler(et::event_loop& loop,
             et::ipc::JsonPeer& peer,
             Workspace& workspace,
             WorkerPool& pool,
             llvm::DenseMap<std::uint32_t, Session>& sessions);
    ~Compiler();


    void init_compile_graph();


    /// Fill compile arguments for a file (CDB lookup + header context fallback).
    /// @param session  If non-null, used for header context resolution on open files.
    bool fill_compile_args(llvm::StringRef path,
                           std::string& directory,
                           std::vector<std::string>& arguments,
                           Session* session = nullptr);


    /// Compile an open file's AST if dirty.  On success, updates session's
    /// file_index, pch_ref, ast_deps, and publishes diagnostics.
    et::task<bool> ensure_compiled(Session& session);


    using RawResult = et::task<et::serde::RawValue, et::ipc::Error>;

    RawResult forward_query(worker::QueryKind kind, Session& session);
    RawResult forward_query(worker::QueryKind kind,
                            const protocol::Position& position,
                            Session& session);

    RawResult forward_build(worker::BuildKind kind,
                            const protocol::Position& position,
                            Session& session);

    RawResult handle_completion(const protocol::Position& position, Session& session);

    void clear_diagnostics(const std::string& uri);

    /// Callback invoked when indexing should be scheduled.
    std::function<void()> on_indexing_needed;

private:
    et::task<bool> ensure_deps(Session& session,
                               const std::string& directory,
                               const std::vector<std::string>& arguments,
                               std::pair<std::string, uint32_t>& pch,
                               std::unordered_map<std::string, std::string>& pcms);

    et::task<bool> ensure_pch(Session& session,
                              const std::string& directory,
                              const std::vector<std::string>& arguments);

    bool is_stale(const Session& session);
    void record_deps(Session& session, llvm::ArrayRef<std::string> deps);

    void publish_diagnostics(const std::string& uri, int version, const et::serde::RawValue& diags);

    std::optional<HeaderFileContext> resolve_header_context(std::uint32_t header_path_id,
                                                            Session* session);

    bool fill_header_context_args(llvm::StringRef path,
                                  std::uint32_t path_id,
                                  std::string& directory,
                                  std::vector<std::string>& arguments,
                                  Session* session);

    et::serde::RawValue complete_include(const PreambleCompletionContext& ctx,
                                         llvm::StringRef path);
    et::serde::RawValue complete_import(const PreambleCompletionContext& ctx);

private:
    et::event_loop& loop;
    et::ipc::JsonPeer& peer;
    Workspace& workspace;
    WorkerPool& pool;
    llvm::DenseMap<std::uint32_t, Session>& sessions;
};

}  // namespace clice

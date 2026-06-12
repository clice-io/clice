#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "command/command.h"
#include "server/service/session.h"
#include "server/worker/worker_pool.h"
#include "server/workspace/workspace.h"
#include "syntax/completion.h"

#include "kota/async/async.h"
#include "kota/codec/json/json.h"
#include "kota/ipc/codec/json.h"
#include "kota/ipc/lsp/protocol.h"
#include "kota/ipc/peer.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"

namespace clice {

namespace protocol = kota::ipc::protocol;

/// Convert a file:// URI to a local file path.
std::string uri_to_path(const std::string& uri);

/// Where the compile command for a file came from. Anything other than
/// CdbExact means the command was guessed to some degree, which is why
/// diagnostics produced with it may deserve a guidance note (see
/// Compiler::publish_diagnostics).
enum class CommandSource : std::uint8_t {
    /// Direct compilation database entry for the file.
    CdbExact,
    /// Header compiled in the context of a host source found through the
    /// include graph (automatic or via clice/switchContext).
    IncludeGraph,
    /// Reserved for command transfer heuristics (e.g. nearest CDB entry);
    /// no producer yet.
    Inferred,
    /// Synthesized default command — no CDB entry and no usable host source.
    Fallback,
};

/// Stable snake_case name of a CommandSource, used in the decision log.
llvm::StringRef command_source_name(CommandSource source);

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
    Compiler(kota::event_loop& loop, Workspace& workspace, WorkerPool& pool);

    void set_peer(kota::ipc::JsonPeer* p) {
        peer = p;
    }

    ~Compiler();

    void init_compile_graph();

    /// Fill compile arguments for a file and report where they came from.
    /// Tries, in order: CDB entry, header context through the include graph,
    /// and finally a synthesized fallback command — so it always succeeds.
    /// Emits a per-file decision log (tiers tried, tier hit, command hash).
    /// @param session  If non-null, used for header context resolution on open files.
    CommandSource fill_compile_args(llvm::StringRef path,
                                    std::string& directory,
                                    std::vector<std::string>& arguments,
                                    Session* session = nullptr);

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

    /// Forward a formatting request to a stateless worker.
    RawResult forward_format(std::shared_ptr<Session> session,
                             std::optional<protocol::Range> range = {});

    /// Handle completion requests.  Detects preamble context (include/import)
    /// and serves those locally; delegates code completion to a stateless worker.
    RawResult handle_completion(const protocol::Position& position,
                                std::shared_ptr<Session> session);

    /// Send an empty diagnostics notification to clear stale markers in the editor.
    void clear_diagnostics(const std::string& uri);

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

    /// Deserialize worker-produced diagnostics and publish them. When the
    /// compile command was not an exact CDB match and the diagnostics contain
    /// file-not-found class errors, a file-top guidance diagnostic explaining
    /// the inferred command is merged into the same publish.
    void publish_diagnostics(const std::string& uri,
                             int version,
                             const kota::codec::RawValue& diags,
                             CommandSource source);

    std::optional<HeaderFileContext> resolve_header_context(std::uint32_t header_path_id,
                                                            Session* session);

    bool fill_header_context_args(llvm::StringRef path,
                                  std::uint32_t path_id,
                                  std::string& directory,
                                  std::vector<std::string>& arguments,
                                  Session* session);

private:
    kota::event_loop& loop;
    kota::ipc::JsonPeer* peer = nullptr;
    Workspace& workspace;
    WorkerPool& pool;
    kota::task_group<> compile_tasks{loop};
};

}  // namespace clice

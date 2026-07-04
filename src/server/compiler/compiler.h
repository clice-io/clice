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
#include "syntax/completion.h"

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

/// Where the compile command for a file came from. Anything other than
/// CDBExact means the command was guessed to some degree, which is why
/// diagnostics produced with it may deserve a guidance note (see
/// format_diagnostics).
enum class CommandSource : std::uint8_t {
    /// Direct compilation database entry for the file.
    CDBExact,
    /// Header compiled in the context of a host source found through the
    /// include graph (automatic or via clice/switchContext).
    IncludeGraph,
    /// Reserved for command transfer heuristics (e.g. nearest CDB entry);
    /// no producer yet.
    Inferred,
    /// Synthesized default command — no CDB entry and no usable host source.
    Fallback,
};

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

    /// Forward a document-link query to the stateful worker holding this
    /// file's AST. Covers the main-file region only: the preamble's links
    /// are cached master-side with the PCH (see PCHState::preamble_links).
    kota::task<std::vector<feature::DocumentLink>, kota::ipc::Error>
        forward_document_links(std::shared_ptr<Session> session);

    /// Forward a formatting request to a stateless worker.
    RawResult forward_format(std::shared_ptr<Session> session,
                             std::optional<protocol::Range> range = {});

    /// Handle completion requests.  Detects preamble context (include/import)
    /// and serves those locally; delegates code completion to a stateless worker.
    RawResult handle_completion(const protocol::Position& position,
                                std::shared_ptr<Session> session);

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

    static void append_suffix_include(const Session& session, std::string& text);

    kota::event_loop& loop;
    Workspace& workspace;
    ContextResolver& contexts;
    WorkerPool& pool;
    kota::task_group<> compile_tasks{loop};
};

/// Format a materialized compile output into publishable diagnostics:
/// deserialize the worker's raw diagnostics, drop phantom suffix-include
/// lines, and — when the compile command was not an exact CDB match and
/// the diagnostics contain file-not-found class errors — merge a file-top
/// guidance diagnostic explaining the inferred command. Formatting is
/// compile semantics; sending the result is the transport's job.
std::vector<protocol::Diagnostic> format_diagnostics(const CompileOutput& output);

/// Convert a compile output's inactive-region byte offsets into LSP ranges
/// using the session's line map.
std::vector<protocol::Range> format_inactive_regions(const Session& session,
                                                     const CompileOutput& output);

}  // namespace clice

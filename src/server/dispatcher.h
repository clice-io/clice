#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "server/ast_family.h"
#include "server/session.h"
#include "worker/pool.h"

#include "kota/async/async.h"
#include "kota/codec/json/json.h"
#include "kota/ipc/lsp/protocol.h"
#include "llvm/ADT/StringRef.h"

namespace clice {

struct EditorContext;

namespace protocol = kota::ipc::protocol;

/// LSP's ContentModified: the request's answer would describe a buffer the
/// client already moved past. Clients keep what they have and re-pull —
/// unlike a null, which a client is entitled to read as "there is nothing"
/// (VS Code clears semantic highlighting on one).
constexpr inline auto content_modified_code =
    static_cast<protocol::integer>(protocol::LSPErrorCodes::ContentModified);

kota::ipc::Error content_modified();

/// The document side of talking to workers. Every request that carries an
/// open document's content to a worker — or asks the worker holding its
/// AST — goes through here and lands through one exit. The pool owns the
/// worker side (slots, crash budgets, routing, attributing a death); this
/// owns what a request means for the document: whether its quarantine
/// admits it, whose crash a death is, and whether the reply still
/// describes the buffer.
///
/// Landing is the invariant: a reply is handed back only while the ticket
/// is fresh, and only a fresh reply settles the document's quarantine
/// record for its kind. A stale reply becomes ContentModified before any
/// caller can see it — no path returns a result for a buffer that no
/// longer exists. A request the workers could not serve — barred by a
/// crash, or lost to deaths and outages — answers empty: the crash note on
/// the document says why, and an error would only surface as a client
/// popup.
class Dispatcher {
public:
    Dispatcher(Project& project, EditorContext& contexts, ASTFamily& ast, WorkerPool& pool);

    using RawResult = kota::task<kota::codec::RawValue, kota::ipc::Error>;

    /// An AST query to the stateful worker holding the file's AST, once the
    /// family compiled it. Position-sensitive queries (hover, goto) pass a
    /// Position; range-sensitive ones (inlay hints) a Range.
    /// `token`, on every dispatch: the LSP request's cancellation token.
    /// Passing it into the worker send turns a client $/cancelRequest into
    /// a wire cancel — the worker stops the parse at the next top-level
    /// declaration instead of computing a result nobody will read. The
    /// shared compile a query waits on is deliberately NOT cancelled: it
    /// serves every waiter, not this request.
    RawResult query(worker::QueryKind kind,
                    const Ticket& ticket,
                    std::optional<protocol::Position> position = {},
                    std::optional<protocol::Range> range = {},
                    kota::cancellation_token token = {});

    /// The main-file document links from the stateful worker holding the
    /// AST; the preamble's live in the PCH envelope (see
    /// PCHState::load_state).
    kota::task<std::vector<index::DocumentLink>, kota::ipc::Error>
        document_links(const Ticket& ticket, kota::cancellation_token token = {});

    /// The folding ranges from the stateful worker holding the AST, none
    /// without one.
    kota::task<std::optional<std::vector<feature::FoldingRange>>, kota::ipc::Error>
        folding_ranges(const Ticket& ticket, kota::cancellation_token token = {});

    /// The code actions on a range of the buffer, from the stateful worker
    /// holding the AST; index requests come back unresolved.
    kota::task<std::vector<feature::CodeAction>, kota::ipc::Error>
        code_actions(const Ticket& ticket,
                     const protocol::Range& range,
                     kota::cancellation_token token = {});

    /// The interactive stateless builds: the buffer and its compile inputs
    /// go to a stateless worker, which compiles a buffer state the shared
    /// AST round may not have seen yet.
    RawResult completion(const Ticket& ticket,
                         const protocol::Position& position,
                         const feature::CompletionClient& client,
                         kota::cancellation_token token = {});
    RawResult signature_help(const Ticket& ticket,
                             const protocol::Position& position,
                             kota::cancellation_token token = {});

    /// Formatting on a stateless worker: no sema runs, but it is still this
    /// document's content on a worker.
    RawResult format(const Ticket& ticket,
                     std::optional<protocol::Range> range = {},
                     kota::cancellation_token token = {});

private:
    /// Shared body of the typed requests to the worker holding the AST
    /// (document links, folding ranges, code actions): the compile, the gate, the send
    /// and the landing; a missing AST answers an empty result.
    template <typename Params>
    kota::task<typename protocol::RequestTraits<Params>::Result, kota::ipc::Error>
        typed(const Ticket& ticket,
              EvidenceKind kind,
              llvm::StringRef label,
              Params params,
              kota::cancellation_token token);

    /// Shared body of the interactive builds: identical inputs and
    /// quarantine passage, different wire type, evidence slot and label.
    /// `wp` arrives with the fields particular to its request filled in.
    template <typename Params>
    RawResult interactive(std::uint8_t evidence,
                          llvm::StringRef label,
                          const Ticket& ticket,
                          protocol::Position position,
                          Params wp,
                          kota::cancellation_token token);

    /// Send an AST query of kind `evidence` to the worker holding the
    /// document, which the caller compiled. A resend after a worker death
    /// recompiles first — the AST died with the worker — and a worker that
    /// no longer holds the document answers document_unloaded, which
    /// compiles it there and asks once more. `unanswered` reports a query
    /// never sent: its kind barred by a crash, or a recompile that produced
    /// no AST.
    template <typename Params>
    RequestResult<Params> ask(const Ticket& ticket,
                              std::uint8_t evidence,
                              const Params& params,
                              kota::cancellation_token token,
                              bool& unanswered);

    /// The single exit of every dispatch: a fresh reply settles the kind's
    /// quarantine record, a stale one never leaves as a value — unless it
    /// is a `snapshot` reply, which describes the buffer the request
    /// carried and which the client reconciles with the edits made
    /// meanwhile. A fresh dispatch the workers could not serve answers
    /// empty.
    template <typename Outcome>
    Outcome land(const Ticket& ticket,
                 std::uint8_t kind,
                 llvm::StringRef label,
                 Outcome result,
                 bool snapshot = false);

    Project& project;
    EditorContext& contexts;
    ASTFamily& ast;
    WorkerPool& pool;
};

}  // namespace clice

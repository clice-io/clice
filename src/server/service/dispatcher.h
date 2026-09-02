#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "server/service/ast_family.h"
#include "server/state/session.h"
#include "worker/pool.h"

#include "kota/async/async.h"
#include "kota/codec/json/json.h"
#include "kota/ipc/lsp/protocol.h"
#include "llvm/ADT/StringRef.h"

namespace clice {

class ContextResolver;

namespace protocol = kota::ipc::protocol;

/// LSP's ContentModified: the request's answer would describe a buffer the
/// client already moved past. Clients keep what they have and re-pull —
/// unlike a null, which a client is entitled to read as "there is nothing"
/// (VS Code clears semantic highlighting on one). kota's ErrorCode has no
/// entry for it.
constexpr inline protocol::integer content_modified_code = -32801;

kota::ipc::Error content_modified();

/// The document side of talking to workers. Every request that carries an
/// open document's content to a worker — or asks the worker holding its
/// AST — goes through here and lands through one exit. The pool owns the
/// worker side (slots, crash budgets, routing); this owns what a request
/// means for the document: whether its quarantine admits it, whose
/// evidence a crash is, and whether the reply still describes the buffer.
///
/// Landing is the invariant: a reply is handed back only while the ticket
/// is fresh, and only a fresh reply settles the document's quarantine
/// ledger for its kind. A stale reply becomes ContentModified before any
/// caller can see it — no path returns a result for a buffer that no
/// longer exists.
class Dispatcher {
public:
    Dispatcher(Workspace& workspace, ContextResolver& contexts, ASTFamily& ast, WorkerPool& pool);

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
                    std::optional<kota::cancellation_token> token = {});

    /// The main-file document links from the stateful worker holding the
    /// AST; the preamble's live in the PCH envelope (see
    /// PCHState::load_state).
    kota::task<std::vector<feature::DocumentLink>, kota::ipc::Error>
        document_links(const Ticket& ticket, std::optional<kota::cancellation_token> token = {});

    /// The interactive stateless builds: the buffer and its compile inputs
    /// go to a stateless worker, which compiles a buffer state the shared
    /// AST round may not have seen yet.
    RawResult completion(const Ticket& ticket,
                         const protocol::Position& position,
                         std::optional<kota::cancellation_token> token = {});
    RawResult signature_help(const Ticket& ticket,
                             const protocol::Position& position,
                             std::optional<kota::cancellation_token> token = {});

    /// Formatting on a stateless worker: no sema runs, but it is still this
    /// document's content on a worker.
    RawResult format(const Ticket& ticket,
                     std::optional<protocol::Range> range = {},
                     std::optional<kota::cancellation_token> token = {});

private:
    /// Shared body of the interactive builds: identical inputs and
    /// quarantine passage, different wire type, evidence slot and label.
    template <typename Params>
    RawResult interactive(std::uint8_t evidence,
                          llvm::StringRef label,
                          const Ticket& ticket,
                          protocol::Position position,
                          std::optional<kota::cancellation_token> token);

    /// A quarantine refusal of a content-carrying build announces the
    /// quarantine, or a completion-only client would never see it.
    kota::ipc::Error refuse(const std::shared_ptr<Session>& session);

    /// The single exit of every dispatch: a fresh reply settles the kind's
    /// ledger, a stale one never leaves as a value.
    template <typename Outcome>
    Outcome land(const Ticket& ticket, std::uint8_t kind, llvm::StringRef label, Outcome result);

    Workspace& workspace;
    ContextResolver& contexts;
    ASTFamily& ast;
    WorkerPool& pool;
};

}  // namespace clice

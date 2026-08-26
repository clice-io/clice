#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "server/service/ast_family.h"
#include "server/state/session.h"
#include "worker/pool.h"

#include "kota/async/async.h"
#include "kota/codec/json/json.h"
#include "kota/ipc/codec/json.h"
#include "kota/ipc/lsp/protocol.h"

namespace clice {

namespace testing {

struct ForwarderFixture;

}

namespace protocol = kota::ipc::protocol;

class ContextResolver;

/// Worker request forwarding: every feature answer produced by a worker
/// process goes through here. Stateful queries ride the AST the family
/// compiled; interactive stateless builds (completion, signature help)
/// prepare their own PCH/PCM inputs, since they compile a buffer state
/// the shared AST round may not have seen yet.
///
/// The forwarder owns the per-request quarantine plumbing (kind ledgers,
/// recovery probes, announcements) — the request is the unit that proves
/// or disproves a kind's evidence.
class WorkerForwarder {
public:
    WorkerForwarder(Workspace& workspace,
                    ContextResolver& contexts,
                    PCMFamily& pcm,
                    PCHFamily& pch,
                    ASTFamily& ast,
                    WorkerPool& pool);

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

    /// Forward a completion request to a stateless worker. Sends the full
    /// buffer content and compile arguments. `token`: see forward_query.
    RawResult forward_completion(const protocol::Position& position,
                                 std::shared_ptr<Session> session,
                                 std::optional<kota::cancellation_token> token = {});

    /// Forward a signature-help request to a stateless worker; same inputs
    /// as completion.
    RawResult forward_signature_help(const protocol::Position& position,
                                     std::shared_ptr<Session> session,
                                     std::optional<kota::cancellation_token> token = {});

    /// Forward a document-link query to the stateful worker holding this
    /// file's AST. Covers the main-file region only: the preamble's links
    /// live in the PCH's pch.idx envelope (see PCHState::load_state).
    /// `token`: see forward_query.
    kota::task<std::vector<feature::DocumentLink>, kota::ipc::Error>
        forward_document_links(std::shared_ptr<Session> session,
                               std::optional<kota::cancellation_token> token = {});

    /// Forward a formatting request to a stateless worker. `token`: see
    /// forward_query.
    RawResult forward_format(std::shared_ptr<Session> session,
                             std::optional<protocol::Range> range = {},
                             std::optional<kota::cancellation_token> token = {});

private:
    /// Shared body of the interactive stateless builds (completion and
    /// signature help): identical inputs and quarantine plumbing, different
    /// wire type, evidence slot and log label.
    template <typename Params>
    RawResult forward_interactive(std::uint8_t evidence,
                                  llvm::StringRef label,
                                  protocol::Position position,
                                  std::shared_ptr<Session> session,
                                  std::optional<kota::cancellation_token> token);

    /// Prepare an interactive build's inputs: module dependencies,
    /// build-or-reuse PCH, and the PCM path table. Under readonly = "on"
    /// the PCH step is skipped — the build compiles without a preamble,
    /// the profile's stated trade.
    /// @param license_generation, license_epoch  The caller's takeoff
    ///               snapshots (session generation, projection epoch),
    ///               taken before its first suspension: the pch_key write
    ///               license. A supersede bumps generation; a Lost-type
    ///               invalidation bumps only the projection epoch — either
    ///               way the resolved command may describe a command that
    ///               no longer exists, and adopting the key would hand
    ///               later incomplete-preamble edits a stale-flag PCH.
    ///               This path runs no graph round it could ask instead.
    kota::task<bool> prepare_inputs(const std::shared_ptr<Session>& session,
                                    std::uint64_t license_generation,
                                    std::uint64_t license_epoch,
                                    const std::string& directory,
                                    const std::vector<std::string>& arguments,
                                    std::pair<std::string, uint32_t>& pch_pair,
                                    std::unordered_map<std::string, std::string>& pcms);

    /// The PCH half of prepare_inputs: plan the key, acquire through the
    /// family, and adopt under the license.
    kota::task<bool> ensure_pch(const std::shared_ptr<Session>& session,
                                std::uint64_t license_generation,
                                std::uint64_t license_epoch,
                                const std::string& directory,
                                const std::vector<std::string>& arguments);

    Workspace& workspace;
    ContextResolver& contexts;
    PCMFamily& pcm;
    PCHFamily& pch;
    ASTFamily& ast;
    WorkerPool& pool;

    friend struct testing::ForwarderFixture;
};

}  // namespace clice

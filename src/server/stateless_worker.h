#pragma once

#include "server/options.h"
#include "server/worker_protocol.h"

#include "eventide/async/async.h"
#include "eventide/ipc/peer.h"

namespace clice {

namespace et = eventide;

class StatelessWorker {
public:
    StatelessWorker(et::event_loop& loop, et::ipc::BincodePeer& peer);

    void register_callbacks();

private:
    using Ctx = et::ipc::BincodePeer::RequestContext;

    et::task<worker::BuildPCHResult, et::ipc::protocol::Error>
    on_build_pch(Ctx& ctx, const worker::BuildPCHParams& params);

    et::task<worker::BuildPCMResult, et::ipc::protocol::Error>
    on_build_pcm(Ctx& ctx, const worker::BuildPCMParams& params);

    et::task<worker::CompletionResult, et::ipc::protocol::Error>
    on_completion(Ctx& ctx, const worker::CompletionParams& params);

    et::task<worker::SignatureHelpResult, et::ipc::protocol::Error>
    on_signature_help(Ctx& ctx, const worker::SignatureHelpParams& params);

    et::task<worker::IndexResult, et::ipc::protocol::Error>
    on_index(Ctx& ctx, const worker::IndexParams& params);

private:
    et::event_loop& loop;
    et::ipc::BincodePeer& peer;
};

int run_stateless_worker_mode(const Options& options);

}  // namespace clice

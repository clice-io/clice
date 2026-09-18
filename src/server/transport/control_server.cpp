#include "server/transport/control_server.h"

#include <format>
#include <list>
#include <memory>

#include "server/protocol/control.h"
#include "server/state/file_tracker.h"
#include "server/transport/master_server.h"
#include "support/logging.h"

#include "kota/ipc/codec/json.h"
#include "kota/ipc/transport.h"
#include "llvm/ADT/SmallVector.h"

namespace clice {

namespace {

using kota::ipc::RequestResult;
using RequestContext = kota::ipc::JsonPeer::RequestContext;

void register_control(MasterServer& srv, kota::ipc::JsonPeer& peer) {
    peer.on_request([&srv](RequestContext&, const control::IndexParams& params)
                        -> RequestResult<control::IndexParams> {
        auto active = srv.workspace.build.active_configuration();
        if(params.configuration != active) {
            co_return kota::outcome_error(kota::ipc::Error{
                std::format("the running clice server indexes configuration '{}', not '{}'",
                            std::string_view(active),
                            params.configuration)});
        }
        if(!srv.workspace.config.project.enable_indexing.value) {
            co_return kota::outcome_error(
                kota::ipc::Error{"the running clice server has background indexing disabled"});
        }
        // Build changes land through the tracker's poll; a request right
        // after a compile_commands.json edit must see the new units.
        if(srv.tracker) {
            auto events = srv.tracker->tick_cdb(/*force=*/true);
            if(!events.empty()) {
                srv.dispatch(events);
            }
        }
        control::IndexResult result;
        llvm::SmallVector<Fid> files;
        for(auto member: srv.workspace.build.members()) {
            if(srv.pump.enqueue(member, ReindexReason::DepsOnly)) {
                files.push_back(member);
            }
        }
        srv.pump.schedule(/*immediate=*/true);
        for(auto file: files) {
            // One await covers one attempt; a crash or preemption
            // requeues the file behind it.
            while(srv.pump.pending_reason(file)) {
                co_await srv.pump.await_attempt(file);
            }
        }
        // The round persists at its end; the asker reads the disk, so its
        // rows must be there before the answer.
        srv.pump.claim_report(co_await srv.index_store.save(srv.pump.save_debt()));
        if(srv.index_store.has_unsaved_state()) {
            co_return kota::outcome_error(
                kota::ipc::Error{"part of the index could not be persisted; see the server log"});
        }
        for(auto file: files) {
            if(srv.pump.failed().contains(file)) {
                result.failed.emplace_back(srv.workspace.file_table.resolve(file));
            }
        }
        co_return result;
    });
}

using Connections = std::list<std::unique_ptr<kota::ipc::JsonPeer>>;

kota::task<> run_connection(kota::ipc::JsonPeer* peer,
                            Connections& connections,
                            Connections::iterator pos) {
    co_await peer->run();
    LOG_DEBUG("Control client disconnected");
    connections.erase(pos);
}

}  // namespace

kota::task<> serve_control(MasterServer& server, kota::tcp::acceptor acceptor) {
    auto& loop = kota::event_loop::current();
    kota::task_group<> group(loop);
    Connections connections;
    group.spawn([](MasterServer& server,
                   kota::tcp::acceptor& acceptor,
                   Connections& connections,
                   kota::task_group<>& group) -> kota::task<> {
        auto& loop = kota::event_loop::current();
        while(true) {
            auto conn = co_await acceptor.accept();
            if(!conn.has_value()) {
                break;
            }
            LOG_DEBUG("Control client connected");
            auto transport = std::make_unique<kota::ipc::StreamTransport>(std::move(*conn));
            auto peer = std::make_unique<kota::ipc::JsonPeer>(loop, std::move(transport));
            register_control(server, *peer);
            auto* peer_ptr = peer.get();
            auto it = connections.emplace(connections.end(), std::move(peer));
            group.spawn(run_connection(peer_ptr, connections, it));
        }
    }(server, acceptor, connections, group));
    co_await group.join();
}

}  // namespace clice

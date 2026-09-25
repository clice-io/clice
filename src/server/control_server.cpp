#include "server/control_server.h"

#include <format>
#include <list>
#include <memory>

#include "server/control.h"
#include "server/file_tracker.h"
#include "server/project_server.h"
#include "support/logging.h"

#include "kota/ipc/codec/json.h"
#include "kota/ipc/transport.h"
#include "llvm/ADT/SmallVector.h"

namespace clice {

namespace {

using kota::ipc::RequestResult;
using RequestContext = kota::ipc::JsonPeer::RequestContext;

void register_control(ProjectServer& srv, kota::ipc::JsonPeer& peer) {
    peer.on_request([&srv](RequestContext&, const control::IndexParams& params)
                        -> RequestResult<control::IndexParams> {
        auto active = srv.project.build.active_configuration();
        if(params.configuration != active) {
            co_return kota::outcome_error(kota::ipc::Error{
                std::format("the running clice server indexes configuration '{}', not '{}'",
                            std::string_view(active),
                            params.configuration)});
        }
        if(!srv.project.config.project.enable_indexing.value) {
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
        auto members = srv.project.build.members();
        if(members.empty()) {
            co_return kota::outcome_error(kota::ipc::Error{
                "nothing to index: the running clice server's build has no translation units"});
        }
        control::IndexResult result;
        llvm::SmallVector<Fid> files;
        for(auto member: members) {
            if(srv.sched.pump.enqueue(member, ReindexReason::DepsOnly)) {
                files.push_back(member);
            }
        }
        srv.sched.pump.schedule(/*immediate=*/true);
        for(auto file: files) {
            // One await covers one attempt; a crash or preemption
            // requeues the file behind it.
            while(srv.sched.pump.pending_reason(file)) {
                co_await srv.sched.pump.await_attempt(file);
            }
        }
        // The round persists at its end; the asker reads the disk, so its
        // rows must be there before the answer.
        srv.sched.pump.claim_report(co_await srv.sched.store.save(srv.sched.pump.save_debt()));
        if(srv.sched.store.has_unsaved_state()) {
            co_return kota::outcome_error(
                kota::ipc::Error{"part of the index could not be persisted; see the server log"});
        }
        // Not current after the sweep: the attempt failed, or the serving
        // side vetoed it (an open buffer diverged from the disk) and the
        // shard still describes older bytes.
        for(auto file: files) {
            auto shard = srv.project.project_index.shards.find(file);
            auto disk = srv.project.file_table.current(file);
            bool current = shard != srv.project.project_index.shards.end() && disk &&
                           shard->second.content_hash() == disk->hash;
            if(!current) {
                result.failed.emplace_back(srv.project.file_table.resolve(file));
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

kota::task<> serve_control(ProjectServer& server, kota::tcp::acceptor acceptor) {
    auto& loop = kota::event_loop::current();
    kota::task_group<> group(loop);
    Connections connections;
    group.spawn([](ProjectServer& server,
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

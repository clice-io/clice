#include "server/transport/control_server.h"

#include <list>
#include <memory>

#include "server/protocol/control.h"
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
    peer.on_request([&srv](RequestContext&,
                           const control::IndexParams&) -> RequestResult<control::IndexParams> {
        control::IndexResult result;
        llvm::SmallVector<Fid> files;
        for(auto member: srv.workspace.build.members()) {
            if(srv.pump.enqueue(member, ReindexReason::DepsOnly)) {
                files.push_back(member);
            }
        }
        srv.pump.schedule(/*immediate=*/true);
        for(auto file: files) {
            co_await srv.pump.await_attempt(file);
        }
        // The round persists at its end; the asker reads the disk, so its
        // rows must be there before the answer.
        srv.pump.claim_report(co_await srv.index_store.save(srv.pump.save_debt()));
        for(auto file: files) {
            if(srv.pump.failed().contains(file)) {
                result.failed.emplace_back(srv.workspace.file_table.resolve(file));
            }
        }
        co_return result;
    });

    peer.on_request([&srv](RequestContext&,
                           const control::StatusParams&) -> RequestResult<control::StatusParams> {
        // The progress numbers describe the current round — or the last
        // one, retained after it ends; the live queue is compacted
        // between rounds and would read as "nothing was ever indexed".
        auto& progress = srv.pump.progress();
        co_return control::StatusResult{
            .idle = srv.pump.is_idle(),
            .pending = static_cast<int>(srv.pump.pending_files()),
            .total = static_cast<int>(progress.total),
            .indexed = static_cast<int>(progress.completed),
        };
    });
}

struct Connection {
    std::unique_ptr<kota::ipc::JsonPeer> peer;
};

kota::task<> run_connection(kota::ipc::JsonPeer* peer,
                            std::list<Connection>& connections,
                            std::list<Connection>::iterator pos) {
    co_await peer->run();
    LOG_DEBUG("Control client disconnected");
    connections.erase(pos);
}

}  // namespace

kota::task<> serve_control(MasterServer& server, kota::tcp::acceptor acceptor) {
    auto& loop = kota::event_loop::current();
    kota::task_group<> group(loop);
    std::list<Connection> connections;
    group.spawn([](MasterServer& server,
                   kota::tcp::acceptor& acceptor,
                   std::list<Connection>& connections,
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
            auto it = connections.emplace(connections.end(), Connection{.peer = std::move(peer)});
            group.spawn(run_connection(peer_ptr, connections, it));
        }
    }(server, acceptor, connections, group));
    co_await group.join();
}

}  // namespace clice

module clice;

import :index.project_index;
import :index.shard;
import :project.save;
import :index.writer_lock;
import :server.control;
import :server.file_tracker;
import :server.project_server;
import :support.logging;

namespace clice::control {

namespace {

using ipc::RequestResult;
using RequestContext = ipc::JSONPeer::RequestContext;

kota::task<> exchange(ipc::JSONPeer& peer,
                      IndexParams params,
                      std::expected<IndexResult, std::string>& out) {
    auto result = co_await peer.send_request(std::move(params));
    if(result) {
        out = std::move(*result);
    } else {
        out = std::unexpected(std::format("request failed: {}", result.error().message));
    }
    peer.close();
}

kota::task<std::expected<IndexResult, std::string>> ask(const index::ServerEndpoint& endpoint,
                                                        IndexParams params) {
    auto& loop = kota::event_loop::current();
    auto transport = co_await ipc::StreamTransport::connect_tcp(endpoint.host, endpoint.port, loop);
    if(!transport) {
        co_return std::unexpected(
            std::format("cannot connect to the clice server (pid {}) at {}:{}",
                        endpoint.pid,
                        endpoint.host,
                        endpoint.port));
    }
    ipc::JSONPeer peer(loop, std::move(*transport));
    std::expected<IndexResult, std::string> out =
        std::unexpected(std::string("the clice server closed the connection"));
    co_await kota::when_all(peer.run(), exchange(peer, std::move(params), out));
    co_return out;
}

void register_requests(ProjectServer& srv, ipc::JSONPeer& peer) {
    peer.on_request([&srv](RequestContext&,
                           const IndexParams& params) -> RequestResult<IndexParams> {
        auto active = srv.project.build.active_configuration();
        if(params.configuration != active) {
            co_await kota::fail(ipc::Error{
                std::format("the running clice server indexes configuration '{}', not '{}'",
                            std::string_view(active),
                            params.configuration)});
        }
        if(!srv.project.config.project.enable_indexing.value) {
            co_await kota::fail(
                ipc::Error{"the running clice server has background indexing disabled"});
        }
        // Build changes arrive through the tracker's poll; a request right
        // after a compile_commands.json edit must see the new units.
        srv.project.file_table.disk.look_flags();
        // The poll restarts the project on the other build; restarted
        // here, it would close this very channel before the answer.
        if(srv.tracker && srv.tracker->moved()) {
            co_await kota::fail(
                ipc::Error{srv.project.config.tracker.workspace_poll_seconds.value > 0
                               ? "the running clice server is about to switch to the build its "
                                 "databases now resolve to; run the command again in a moment"
                               : "the databases of the running clice server now resolve to another "
                                 "build, which it switches to when it starts again: polling is off "
                                 "(tracker.workspace_poll_seconds = 0)"});
        }
        srv.weigh_databases(/*force=*/true);
        auto members = srv.project.build.members();
        if(members.empty()) {
            co_await kota::fail(ipc::Error{
                "nothing to index: the running clice server's build has no translation units"});
        }
        IndexResult result;
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
        // index must be there before the answer.
        co_await srv.sched.pump.persist();
        if(has_unsaved_state(srv.project)) {
            co_await kota::fail(
                ipc::Error{"part of the index could not be persisted; see the server log"});
        }
        // Not current after the pass: the attempt failed, or the serving
        // side vetoed it (an open buffer diverged from the disk) and the
        // shard still describes older bytes.
        for(auto file: files) {
            auto* shard = srv.project.index.shard(file);
            auto disk = srv.project.file_table.disk.current(file);
            bool current = shard && disk && shard->content_hash() == disk->hash;
            if(!current) {
                result.failed.emplace_back(srv.project.file_table.display(file));
            }
        }
        co_return result;
    });
}

using Connections = std::list<std::unique_ptr<ipc::JSONPeer>>;

kota::task<> run_connection(ipc::JSONPeer* peer,
                            Connections& connections,
                            Connections::iterator pos) {
    co_await peer->run();
    LOG_DEBUG("Control client disconnected");
    connections.erase(pos);
}

}  // namespace

std::expected<IndexResult, std::string> request_index(const index::ServerEndpoint& endpoint,
                                                      llvm::StringRef configuration) {
    auto [ended] = kota::run(ask(endpoint, IndexParams{.configuration = configuration.str()}));
    return std::move(*ended);
}

kota::task<> serve(ProjectServer& server, kota::tcp::acceptor acceptor) {
    kota::task_group<> group;
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
            auto transport = std::make_unique<ipc::StreamTransport>(std::move(*conn));
            auto peer = std::make_unique<ipc::JSONPeer>(loop, std::move(transport));
            register_requests(server, *peer);
            auto* peer_ptr = peer.get();
            auto it = connections.emplace(connections.end(), std::move(peer));
            group.spawn(run_connection(peer_ptr, connections, it));
        }
    }(server, acceptor, connections, group));
    co_await group.join();
}

}  // namespace clice::control

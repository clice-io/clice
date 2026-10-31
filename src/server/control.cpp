module clice;

import :index.project_index;
import :index.shard;
import :project.save;
import :index.writer_lock;
import :server.control;
import :server.master;
import :server.served_project;
import :support.logging;

namespace clice::control {

namespace {

using ipc::RequestResult;
using RequestContext = ipc::JSONPeer::RequestContext;

async::task<> exchange(ipc::JSONPeer& peer,
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

async::task<std::expected<IndexResult, std::string>> ask(const index::ServerEndpoint& endpoint,
                                                         IndexParams params) {
    auto& loop = async::event_loop::current();
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
    co_await async::when_all(peer.run(), exchange(peer, std::move(params), out));
    co_return out;
}

void register_requests(ServedProject& srv, ipc::JSONPeer& peer) {
    peer.on_request(
        [&srv](RequestContext&, const IndexParams& params) -> RequestResult<IndexParams> {
            auto active = srv.project.build.active_configuration();
            if(params.configuration != active) {
                co_await async::fail(ipc::Error{
                    std::format("the running clice server indexes configuration '{}', not '{}'",
                                std::string_view(active),
                                params.configuration)});
            }
            if(!srv.project.settings.project.enable_indexing.value) {
                co_await async::fail(
                    ipc::Error{"the running clice server has background indexing disabled"});
            }
            // Build changes arrive through the databases' poll; a request right
            // after a compile_commands.json edit must see the new units.
            srv.project.file_table.disk.look_flags();
            srv.reload_databases(/*force=*/true);
            // Indexing would fill the old build's index while a --fresh answer
            // then opens the new one's.
            if(srv.databases_moved()) {
                co_await async::fail(ipc::Error{
                    "the compilation databases of the running clice server now resolve to another "
                    "build, which it opens once it restarts"});
            }
            auto members = srv.project.build.members();
            if(members.empty()) {
                co_await async::fail(ipc::Error{
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
                co_await async::fail(
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

}  // namespace

std::expected<IndexResult, std::string> request_index(const index::ServerEndpoint& endpoint,
                                                      llvm::StringRef configuration) {
    auto [ended] = async::run(ask(endpoint, IndexParams{.configuration = configuration.str()}));
    return std::move(*ended);
}

async::task<> serve(ServedProject& project, async::tcp::acceptor acceptor) {
    co_await accept_peers(std::move(acceptor), "Control client", [&project](ipc::JSONPeer& peer) {
        register_requests(project, peer);
        return std::shared_ptr<void>();
    });
}

}  // namespace clice::control

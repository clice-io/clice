module clice;

import :index.project_index;
import :index.shard;
import :index.database.writer_lock;
import :server.control;
import :server.master;
import :server.served_project;
import :support.logging;
import :support.process;
import :vfs.path;

namespace clice::control {

namespace {

using ipc::RequestResult;
using RequestContext = ipc::JSONPeer::RequestContext;

/// How long a server may take to accept the connection and answer its
/// status: one answering that much late is taken for hung.
chrono::milliseconds answer_bound() {
    return chrono::milliseconds(env_integer("CLICE_TEST_CONTROL_WAIT_MS").value_or(10'000));
}

async::task<> exchange(ipc::JSONPeer& peer,
                       const index::ServerEndpoint& endpoint,
                       IndexParams params,
                       std::expected<IndexResult, AskError>& out) {
    // Hang detection: a server that does not answer its status is taken for
    // hung.
    if(auto status = co_await peer.send_request(StatusParams{}, {.timeout = answer_bound()});
       !status) {
        out = std::unexpected(AskError{
            .message = std::format("the clice server (pid {}) does not answer", endpoint.pid),
            .unanswered = true});
        peer.close();
        co_return;
    }
    // LIMITATION: a server hanging after it answered its status keeps the
    // asker waiting for good: a pass may take as long as the build has
    // units, so the request itself has no bound.
    auto result = co_await peer.send_request(std::move(params));
    if(result) {
        out = std::move(*result);
    } else {
        out = std::unexpected(
            AskError{.message = std::format("request failed: {}", result.error().message)});
    }
    peer.close();
}

async::task<std::expected<IndexResult, AskError>> ask(const index::ServerEndpoint& endpoint,
                                                      IndexParams params) {
    auto& loop = async::event_loop::current();
    // Hang detection: a server that does not accept is taken for hung.
    auto connected =
        co_await async::with_timeout(async::pipe::connect(endpoint.socket, loop), answer_bound());
    if(connected.is_cancelled()) {
        co_return std::unexpected(AskError{
            .message = std::format("the clice server (pid {}) does not answer", endpoint.pid),
            .unanswered = true});
    }
    if(!connected.has_value()) {
        co_return std::unexpected(
            AskError{.message = std::format("cannot connect to the clice server (pid {}) at {}",
                                            endpoint.pid,
                                            endpoint.socket)});
    }
    ipc::JSONPeer peer(loop, std::make_unique<ipc::StreamTransport>(std::move(*connected)));
    std::expected<IndexResult, AskError> out =
        std::unexpected(AskError{.message = "the clice server closed the connection"});
    // The exchange ends the reading: a server that stopped answering never
    // closes its end.
    co_await async::when_any(peer.run(), exchange(peer, endpoint, std::move(params), out));
    co_return out;
}

void register_requests(ServedProject& srv, ipc::JSONPeer& peer) {
    peer.on_request([](RequestContext&, const StatusParams&) -> RequestResult<StatusParams> {
        co_return StatusResult{};
    });
    peer.on_request(
        [&srv](RequestContext&, const IndexParams& params) -> RequestResult<IndexParams> {
            auto active = srv.project.rules.configuration();
            if(params.configuration != active) {
                co_await async::fail(ipc::Error{
                    std::format("the running clice server indexes configuration '{}', not '{}'",
                                std::string_view(active),
                                params.configuration)});
            }
            auto& background = srv.project.background_index;
            // Build changes arrive through the databases' poll; a request right
            // after a compile_commands.json edit must see the new units.
            srv.project.file_table.disk.look_flags();
            srv.reload_databases(/*force=*/true);
            // Indexing would fill the old build's index while a --fresh answer
            // then opens the new one's.
            if(srv.project.databases_moved()) {
                co_await async::fail(ipc::Error{
                    "the compilation databases of the running clice server now resolve to another "
                    "build, which it opens once it restarts"});
            }
            auto members = srv.project.members();
            if(members.empty()) {
                co_await async::fail(ipc::Error{
                    "nothing to index: the running clice server's build has no translation units"});
            }
            IndexResult result;
            llvm::SmallVector<Fid> files;
            for(auto member: members) {
                if(background.enqueue(member, ReindexReason::DepsOnly)) {
                    files.push_back(member);
                }
            }
            background.schedule(/*immediate=*/true);
            for(auto file: files) {
                // One await covers one attempt; a crash or preemption
                // requeues the file behind it.
                while(background.queue().pending_reason(file)) {
                    co_await background.await_attempt(file);
                }
            }
            // The pass saves at its end; the asker reads the disk, so its
            // index must be there before the answer.
            co_await srv.project.save();
            if(srv.project.has_unsaved_state()) {
                co_await async::fail(
                    ipc::Error{"part of the index could not be persisted; see the server log"});
            }
            // Not current after the pass: the attempt failed, or the serving
            // side vetoed it (an open buffer diverged from the disk) and the
            // shard still describes older bytes.
            // LIMITATION: only the members themselves are checked, not what
            // they include: with a member open (its run vetoed) and a header it
            // includes changed on disk, the run succeeds without the header's
            // new symbols; later queries list the header as stale.
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

std::expected<IndexResult, AskError> request_index(const index::ServerEndpoint& endpoint,
                                                   llvm::StringRef configuration) {
    auto [ended] = async::run(ask(endpoint, IndexParams{.configuration = configuration.str()}));
    return std::move(*ended);
}

std::expected<Listener, std::string> listen(llvm::StringRef cache_dir, async::event_loop& loop) {
    std::string name;
    std::optional<fs::TempDirectory> directory;
#ifdef _WIN32
    // One name per project: a server serves several. The default DACL of a
    // pipe lets other users read it alone, never write a request.
    static unsigned serial = 0;
    name = std::format(R"(\\.\pipe\clice-{}-{})", sys::pid(), serial);
    serial += 1;
#else
    // sun_path holds 104 bytes on macOS, 108 on Linux.
    constexpr std::size_t socket_path_max = 100;
    name = path::join(cache_dir, "server.sock");
    if(name.size() >= socket_path_max) {
        std::string temp;
        sys::temp_dir(temp);
        auto made = fs::TempDirectory::create(temp, "clice-");
        if(!made) {
            return std::unexpected(std::format("cannot make a directory for its socket in {}: {}",
                                               temp,
                                               made.error().message()));
        }
        name = path::join(made->path(), "server.sock");
        directory = std::move(*made);
    }
    // A server that died left its socket: the writer lock this one holds
    // says nothing listens there.
    fs::remove(name);
#endif
    auto acceptor = async::pipe::listen(name, {.no_truncate = true}, loop);
    if(!acceptor) {
        return std::unexpected(
            std::format("cannot listen on {}: {}", name, acceptor.error().message()));
    }
#ifndef _WIN32
    // Connecting takes writing the socket: its user's alone.
    fs::set_permissions(name, 0600);
#endif
    return Listener{.acceptor = std::move(*acceptor),
                    .name = std::move(name),
                    .directory = std::move(directory)};
}

async::task<> serve(ServedProject& project, async::pipe::acceptor acceptor) {
    co_await accept_peers(std::move(acceptor), "Control client", [&project](ipc::JSONPeer& peer) {
        register_requests(project, peer);
        return std::shared_ptr<void>();
    });
}

}  // namespace clice::control

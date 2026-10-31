module;

#include "version.h"

module clice;

import :server.client;
import :server.master;
import :support.logging;
import :support.process;

namespace clice {

/// Runs `serving` until the shutdown token fires or it ends on its own. A
/// cancelled request still waits for its worker's answer, so the server
/// stops — its pool first, killing a worker that would never answer —
/// before `serving` is joined.
static kota::task<> serve_until_shutdown(MasterServer& server, kota::task<> serving) {
    auto watch = [](MasterServer& server, kota::task<> serving) -> kota::task<> {
        co_await kota::with_token(std::move(serving), server.shutdown_token());
        server.schedule_shutdown();
    };
    kota::task_group<> group;
    group.spawn(watch(server, std::move(serving)));
    co_await server.shutdown_token().wait().catch_cancel();
    co_await server.stop();
    co_await group.join();
}

/// Serves until the client, the transport or a signal ends it, then shuts
/// the server down. A termination signal takes the path of an LSP exit.
static kota::task<> serve(MasterServer& server, kota::task<> serving) {
    bool terminating = false;
    auto terminate = [&server] {
        LOG_INFO("Termination signal received; shutting down");
        server.schedule_shutdown();
    };
    kota::task_group<> signals;
    signals.spawn(watch_termination(SIGTERM, terminating, terminate));
    signals.spawn(watch_termination(SIGINT, terminating, terminate));
#ifndef _WIN32
    signals.spawn(watch_termination(SIGHUP, terminating, terminate));
#endif
    co_await serve_until_shutdown(server, std::move(serving));
    co_await server.close();
    signals.cancel();
    co_await signals.join();
}

/// The client's transport, reporting the end of its input as soon as the
/// read sees it: a client that went away without `exit` must not wait for
/// the request handlers a hung worker keeps running.
class InputEndWatch final : public ipc::Transport {
public:
    InputEndWatch(std::unique_ptr<ipc::Transport> inner, std::function<void()> on_end) :
        inner(std::move(inner)), on_end(std::move(on_end)) {}

    kota::task<std::string, ipc::ReadError> read_message() override {
        auto message = co_await inner->read_message();
        if(message.has_error()) {
            if(message.error().kind != ipc::ReadError::Kind::Oversized) {
                on_end();
            }
            co_await kota::fail(std::move(message).error());
        }
        co_return std::move(*message);
    }

    kota::task<void, ipc::Error> write_message(std::string_view payload) override {
        co_await inner->write_message(payload).or_fail();
    }

    kota::task<void, ipc::Error> write_messages(std::span<const std::string> payloads) override {
        co_await inner->write_messages(payloads).or_fail();
    }

    kota::task<void, ipc::Error> close_output() override {
        co_await inner->close_output().or_fail();
    }

    ipc::Result<void> close() override {
        return inner->close();
    }

    std::size_t remote_max_payload() const noexcept override {
        return inner->remote_max_payload();
    }

private:
    std::unique_ptr<ipc::Transport> inner;
    std::function<void()> on_end;
};

kota::task<> accept_peers(kota::tcp::acceptor acceptor,
                          llvm::StringRef what,
                          llvm::unique_function<std::shared_ptr<void>(ipc::JSONPeer&)> accepted) {
    struct Connection {
        std::unique_ptr<ipc::JSONPeer> peer;

        /// What serves the peer, destroyed before it.
        std::shared_ptr<void> served;
    };

    using Connections = std::list<Connection>;
    auto run = [](ipc::JSONPeer* peer,
                  llvm::StringRef what,
                  Connections& connections,
                  Connections::iterator pos) -> kota::task<> {
        co_await peer->run();
        LOG_INFO("{} disconnected", what);
        connections.erase(pos);
    };
    auto& loop = kota::event_loop::current();
    Connections connections;
    // The loop is a child of the group: a cancel reaches it through the
    // group, which waits for every connection's run to end before the peers
    // go.
    co_await kota::with_task_group([&](kota::task_group<>& group) -> kota::task<> {
        while(true) {
            auto connection = co_await acceptor.accept();
            if(!connection.has_value()) {
                break;
            }
            LOG_INFO("{} connected", what);
            auto transport = std::make_unique<ipc::StreamTransport>(std::move(*connection));
            auto peer = std::make_unique<ipc::JSONPeer>(loop, std::move(transport));
            auto* raw = peer.get();
            auto served = accepted(*raw);
            auto pos = connections.emplace(
                connections.end(),
                Connection{.peer = std::move(peer), .served = std::move(served)});
            group.spawn(run(raw, what, connections, pos));
        }
    });
}

int run_serve_mode(const ServerOptions& opts, llvm::StringRef configuration) {
    logging::options.never_block_stderr = true;
    logging::stderr_logger("master", logging::options);

    auto mode = opts.mode.value_or(ServerMode::Pipe);
    auto host = opts.host.value_or("127.0.0.1");
    auto port = opts.port.value_or(0);
    auto record = opts.record.value_or("");
    auto ws = opts.workspace.value_or("");

    LOG_INFO("clice master starting: version={}, target={}, pid={}, mode={}, workspace={}",
             clice::version,
             clice::target,
             sys::pid(),
             mode == ServerMode::Pipe ? "pipe" : "socket",
             ws.empty() ? "<from LSP initialize>" : ws);

    if(mode == ServerMode::Socket && (port <= 0 || port > 65535)) {
        LOG_ERROR("--port must be between 1 and 65535 in socket mode");
        return 1;
    }

    kota::event_loop loop;
    MasterServer server(loop, configuration.str());

    if(mode == ServerMode::Pipe) {
        auto transport = ipc::StreamTransport::open_stdio(loop);
        if(!transport) {
            LOG_ERROR("failed to open stdio transport");
            return 1;
        }

        std::unique_ptr<ipc::Transport> final_transport = std::move(*transport);
        if(!record.empty()) {
            final_transport =
                std::make_unique<ipc::RecordingTransport>(std::move(final_transport), record);
        }
        final_transport = std::make_unique<InputEndWatch>(std::move(final_transport), [&server] {
            server.schedule_shutdown();
        });

        server.client_on_host = true;
        ipc::JSONPeer lsp_peer(loop, std::move(final_transport));
        LSPClient lsp_client(server, lsp_peer);

        loop.schedule(
            [](MasterServer& server, ipc::JSONPeer& peer, std::string root) -> kota::task<> {
                // Pre-initialize for standalone (no-editor) use; LSP initialize
                // will be rejected. Runs inside the loop — before the peer
                // reads its first message — because initialize() spawns
                // background tasks that need the running loop context.
                if(!root.empty()) {
                    server.change_folders({}, {Spelling(root, Spelling::cwd())});
                    server.initialize();
                }
                co_await serve(server, peer.run());
            }(server, lsp_peer, ws));
        loop.run();
        return server.exit_before_shutdown ? 1 : 0;
    }

    // Socket mode: the first connection gets the client, later ones only a
    // peer, and the server takes none after it goes.
    auto acceptor = kota::tcp::listen(host, port, {}, loop);
    if(!acceptor) {
        LOG_ERROR("failed to listen on {}:{}", host, port);
        return 1;
    }
    LOG_INFO("Listening on {}:{} ...", host, port);
    loop.schedule(
        [](MasterServer& server, kota::tcp::acceptor acceptor, std::string root) -> kota::task<> {
            // See the pipe-mode comment: pre-initialization must run
            // inside the loop.
            if(!root.empty()) {
                server.change_folders({}, {Spelling(root, Spelling::cwd())});
                server.initialize();
            }
            bool taken = false;
            co_await serve(
                server,
                accept_peers(std::move(acceptor), "Client", [&server, &taken](ipc::JSONPeer& peer) {
                    std::shared_ptr<void> client;
                    if(!std::exchange(taken, true)) {
                        client = std::make_shared<LSPClient>(server, peer);
                    }
                    return client;
                }));
        }(server, std::move(*acceptor), ws));
    loop.run();
    return server.exit_before_shutdown ? 1 : 0;
}

}  // namespace clice

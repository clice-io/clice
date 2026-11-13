module clice;

import :server.client;
import :server.master;
import :support.logging;
import :support.process;
import :support.version;

namespace clice {

namespace {

/// The editor's transport, every message both ways traced (logging::trace_lsp).
struct TracedTransport : ipc::Transport {
    explicit TracedTransport(std::unique_ptr<ipc::Transport> inner) : inner(std::move(inner)) {
        logging::begin_lsp_trace();
    }

    async::task<std::string, ipc::ReadError> read_message() override {
        auto message = co_await inner->read_message().or_fail();
        logging::trace_lsp(true, message);
        co_return message;
    }

    async::task<void, ipc::Error> write_message(std::string_view payload) override {
        logging::trace_lsp(false, payload);
        co_await inner->write_message(payload).or_fail();
    }

    async::task<void, ipc::Error> write_messages(std::span<const std::string> payloads) override {
        for(auto& payload: payloads) {
            logging::trace_lsp(false, payload);
        }
        co_await inner->write_messages(payloads).or_fail();
    }

    async::task<void, ipc::Error> close_output() override {
        co_await inner->close_output().or_fail();
    }

    ipc::Result<void> close() override {
        return inner->close();
    }

    std::size_t remote_max_payload() const noexcept override {
        return inner->remote_max_payload();
    }

    std::unique_ptr<ipc::Transport> inner;
};

}  // namespace

/// Runs `serving` until the shutdown token fires or it ends on its own. A
/// cancelled request still waits for its worker's answer, so the server
/// stops — its pool first, killing a worker that would never answer —
/// before `serving` is joined.
static async::task<> serve_until_shutdown(MasterServer& server, async::task<> serving) {
    auto watch = [](MasterServer& server, async::task<> serving) -> async::task<> {
        co_await async::with_token(std::move(serving), server.shutdown_token());
        server.schedule_shutdown();
    };
    async::task_group<> group;
    group.spawn(watch(server, std::move(serving)));
    co_await server.shutdown_token().wait().catch_cancel();
    co_await server.stop();
    co_await group.join();
}

/// Serves until the client, the transport or a signal ends it, then shuts
/// the server down. A termination signal takes the path of an LSP exit, and
/// so does the end of the conversation with `client`, the editor's peer in
/// pipe mode, as soon as it ends: a client that went away without `exit`
/// must not wait for the request handlers a hung worker keeps running.
static async::task<> serve(MasterServer& server,
                           async::task<> serving,
                           ipc::JSONPeer* client = nullptr) {
    bool terminating = false;
    auto terminate = [&server] {
        LOG_INFO("Termination signal received; shutting down");
        server.schedule_shutdown();
    };
    auto watch_end = [](MasterServer& server, ipc::JSONPeer& client) -> async::task<> {
        auto end = co_await client.wait_ended();
        if(end.kind != ipc::ConversationEnd::Kind::ShutDown) {
            server.schedule_shutdown();
        }
    };
    async::task_group<> signals;
    signals.spawn(watch_termination(SIGTERM, terminating, terminate));
    signals.spawn(watch_termination(SIGINT, terminating, terminate));
    signals.spawn(watch_termination(sys::sighup, terminating, terminate));
    if(client) {
        signals.spawn(watch_end(server, *client));
    }
    co_await serve_until_shutdown(server, std::move(serving));
    co_await server.close();
    signals.cancel();
    co_await signals.join();
}

async::task<> accept_peers(async::tcp::acceptor acceptor,
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
                  Connections::iterator pos) -> async::task<> {
        co_await peer->run();
        LOG_INFO("{} disconnected", what);
        connections.erase(pos);
    };
    auto& loop = async::event_loop::current();
    Connections connections;
    // The loop is a child of the group: a cancel reaches it through the
    // group, which waits for every connection's run to end before the peers
    // go.
    co_await async::with_task_group([&](async::task_group<>& group) -> async::task<> {
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
    logging::stderr_logger("master", logging::StderrReader::Editor);

    LOG_INFO("clice {} ({}) starting, pid {}, over {}{}",
             clice::version,
             clice::target,
             sys::pid(),
             opts.mode == ServerMode::Pipe ? "stdio" : "a socket",
             opts.workspace.empty() ? std::string()
                                    : std::format(", for the project root {}", opts.workspace));
    if(auto source = built_from(); !source.empty()) {
        LOG_INFO("{}", source);
    }

    if(opts.mode == ServerMode::Socket && (opts.port <= 0 || opts.port > 65535)) {
        LOG_ERROR("--port must be between 1 and 65535 in socket mode");
        return 1;
    }

    async::event_loop loop;
    MasterServer server(loop, configuration.str());

    if(opts.mode == ServerMode::Pipe) {
        auto transport = ipc::StreamTransport::open_stdio(loop);
        if(!transport) {
            LOG_ERROR("failed to open stdio transport");
            return 1;
        }

        std::unique_ptr<ipc::Transport> editor = std::move(*transport);
        if(logging::lsp_trace_enabled()) {
            editor = std::make_unique<TracedTransport>(std::move(editor));
        }

        server.client_on_host = true;
        ipc::JSONPeer lsp_peer(loop, std::move(editor));
        LSPClient lsp_client(server, lsp_peer);

        loop.schedule(
            [](MasterServer& server, ipc::JSONPeer& peer, std::string root) -> async::task<> {
                // Pre-initialize for standalone (no-editor) use; LSP initialize
                // will be rejected. Runs inside the loop — before the peer
                // reads its first message — because initialize() spawns
                // background tasks that need the running loop context.
                if(!root.empty()) {
                    server.change_folders({}, {Spelling(root, Spelling::cwd())});
                    server.initialize();
                }
                co_await serve(server, peer.run(), &peer);
            }(server, lsp_peer, opts.workspace));
        loop.run();
        return server.exit_before_shutdown ? 1 : 0;
    }

    // Socket mode: the first connection gets the client, later ones only a
    // peer, and the server takes none after it goes.
    auto acceptor = async::tcp::listen(opts.host, opts.port, {}, loop);
    if(!acceptor) {
        LOG_ERROR("failed to listen on {}:{}", opts.host, opts.port);
        return 1;
    }
    LOG_INFO("Listening on {}:{} ...", opts.host, opts.port);
    loop.schedule(
        [](MasterServer& server, async::tcp::acceptor acceptor, std::string root) -> async::task<> {
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
        }(server, std::move(*acceptor), opts.workspace));
    loop.run();
    return server.exit_before_shutdown ? 1 : 0;
}

}  // namespace clice

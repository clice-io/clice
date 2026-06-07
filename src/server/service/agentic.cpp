#include "server/service/agentic.h"

#include <memory>
#include <print>
#include <string>

#include "server/protocol/agentic.h"
#include "support/filesystem.h"
#include "support/logging.h"

#include "kota/async/async.h"
#include "kota/ipc/codec/json.h"
#include "kota/ipc/transport.h"

namespace clice {

template <typename Params>
static kota::task<bool> send_and_print(kota::ipc::JsonPeer& peer, Params params) {
    auto result = co_await peer.send_request(std::move(params));
    if(!result) {
        LOG_ERROR("request failed: {}", result.error().message);
        co_return false;
    }
    auto json = kota::codec::json::to_string<kota::ipc::lsp_config>(*result);
    std::println("{}", json ? *json : "null");
    co_return true;
}

static kota::task<> agentic_request(kota::ipc::JsonPeer& peer,
                                    int& exit_code,
                                    const QueryOptions& opts) {
    auto method = opts.method.value_or("compileCommand");
    auto path = opts.path.value_or("");
    auto name = opts.name.value_or("");
    auto query = opts.query.value_or("");
    auto line = opts.line.value_or(0);
    auto direction = opts.direction.value_or("");

    auto opt_name = name.empty() ? std::nullopt : std::optional(name);
    auto opt_path = path.empty() ? std::nullopt : std::optional(path);
    auto opt_line = line > 0 ? std::optional(line) : std::nullopt;
    auto opt_dir = direction.empty() ? std::nullopt : std::optional(direction);

    bool ok = false;

    if(method == "compileCommand") {
        ok = co_await send_and_print(peer, agentic::CompileCommandParams{.path = path});
    } else if(method == "projectFiles") {
        auto filter = query.empty() ? std::nullopt : std::optional(query);
        ok = co_await send_and_print(peer, agentic::ProjectFilesParams{.filter = filter});
    } else if(method == "symbolSearch") {
        ok = co_await send_and_print(peer, agentic::SymbolSearchParams{.query = query});
    } else if(method == "definition") {
        ok = co_await send_and_print(
            peer,
            agentic::DefinitionParams{.name = opt_name, .path = opt_path, .line = opt_line});
    } else if(method == "references") {
        ok = co_await send_and_print(
            peer,
            agentic::ReferencesParams{.name = opt_name, .path = opt_path, .line = opt_line});
    } else if(method == "readSymbol") {
        ok = co_await send_and_print(
            peer,
            agentic::ReadSymbolParams{.name = opt_name, .path = opt_path, .line = opt_line});
    } else if(method == "documentSymbols") {
        ok = co_await send_and_print(peer, agentic::DocumentSymbolsParams{.path = path});
    } else if(method == "callGraph") {
        ok = co_await send_and_print(peer,
                                     agentic::CallGraphParams{
                                         .name = opt_name,
                                         .path = opt_path,
                                         .line = opt_line,
                                         .direction = opt_dir,
                                     });
    } else if(method == "typeHierarchy") {
        ok = co_await send_and_print(peer,
                                     agentic::TypeHierarchyParams{
                                         .name = opt_name,
                                         .path = opt_path,
                                         .line = opt_line,
                                         .direction = opt_dir,
                                     });
    } else if(method == "fileDeps") {
        ok = co_await send_and_print(peer,
                                     agentic::FileDepsParams{.path = path, .direction = opt_dir});
    } else if(method == "impactAnalysis") {
        ok = co_await send_and_print(peer, agentic::ImpactAnalysisParams{.path = path});
    } else if(method == "status") {
        ok = co_await send_and_print(peer, agentic::StatusParams{});
    } else if(method == "shutdown") {
        peer.send_notification(agentic::ShutdownParams{});
        ok = true;
    } else {
        LOG_ERROR("unknown agentic method '{}'", method);
    }

    if(ok)
        exit_code = 0;
    peer.close();
}

static kota::task<> agentic_client(int& exit_code,
                                   std::unique_ptr<kota::ipc::JsonPeer>& peer_out,
                                   const QueryOptions& opts) {
    auto& loop = kota::event_loop::current();
    auto host = opts.host.value_or("127.0.0.1");
    auto port = *opts.port;
    auto transport = co_await kota::ipc::StreamTransport::connect_tcp(host, port, loop);
    if(!transport) {
        LOG_ERROR("failed to connect to {}:{}", host, port);
        co_return;
    }

    peer_out = std::make_unique<kota::ipc::JsonPeer>(loop, std::move(*transport));
    co_await kota::when_all(peer_out->run(), agentic_request(*peer_out, exit_code, opts));
}

int run_agentic_mode(const QueryOptions& opts) {
    logging::stderr_logger("agentic", logging::options);

    kota::event_loop loop;
    int exit_code = 1;
    std::unique_ptr<kota::ipc::JsonPeer> peer;
    loop.schedule(agentic_client(exit_code, peer, opts));
    loop.run();
    return exit_code;
}

static kota::task<> relay_forward(kota::ipc::Transport& from, kota::ipc::Transport& to) {
    while(true) {
        auto msg = co_await from.read_message();
        if(!msg)
            break;
        co_await to.write_message(*msg);
    }
    to.close();
}

static kota::task<> relay_main(kota::event_loop& loop, int& exit_code, std::string socket_path) {
    auto stdio = kota::ipc::StreamTransport::open_stdio(loop);
    if(!stdio) {
        LOG_ERROR("failed to open stdio transport");
        loop.stop();
        co_return;
    }

    auto conn = co_await kota::pipe::connect(socket_path, {}, loop);
    if(!conn) {
        LOG_ERROR("failed to connect to {}", socket_path);
        loop.stop();
        co_return;
    }

    auto socket = std::make_unique<kota::ipc::StreamTransport>(std::move(*conn));

    co_await kota::when_all(relay_forward(**stdio, *socket), relay_forward(*socket, **stdio));
    exit_code = 0;
    loop.stop();
}

int run_relay_mode(llvm::StringRef socket_path) {
    logging::stderr_logger("relay", logging::options);

    auto path = socket_path.empty() ? path::default_socket_path() : socket_path.str();

    kota::event_loop loop;
    int exit_code = 1;
    loop.schedule(relay_main(loop, exit_code, std::move(path)));
    loop.run();
    return exit_code;
}

}  // namespace clice

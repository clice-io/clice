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
                                    const AgenticQueryOptions& opts) {
    bool ok = false;

    if(opts.method == "compileCommand") {
        ok = co_await send_and_print(peer, agentic::CompileCommandParams{.path = opts.path});
    } else if(opts.method == "projectFiles") {
        auto filter = opts.query.empty() ? std::nullopt : std::optional(opts.query);
        ok = co_await send_and_print(peer, agentic::ProjectFilesParams{.filter = filter});
    } else if(opts.method == "symbolSearch") {
        ok = co_await send_and_print(peer, agentic::SymbolSearchParams{.query = opts.query});
    } else if(opts.method == "definition") {
        auto name = opts.name.empty() ? std::nullopt : std::optional(opts.name);
        auto path = opts.path.empty() ? std::nullopt : std::optional(opts.path);
        auto line = opts.line > 0 ? std::optional(opts.line) : std::nullopt;
        ok = co_await send_and_print(
            peer,
            agentic::DefinitionParams{.name = name, .path = path, .line = line});
    } else if(opts.method == "references") {
        auto name = opts.name.empty() ? std::nullopt : std::optional(opts.name);
        auto path = opts.path.empty() ? std::nullopt : std::optional(opts.path);
        auto line = opts.line > 0 ? std::optional(opts.line) : std::nullopt;
        ok = co_await send_and_print(
            peer,
            agentic::ReferencesParams{.name = name, .path = path, .line = line});
    } else if(opts.method == "readSymbol") {
        auto name = opts.name.empty() ? std::nullopt : std::optional(opts.name);
        auto path = opts.path.empty() ? std::nullopt : std::optional(opts.path);
        auto line = opts.line > 0 ? std::optional(opts.line) : std::nullopt;
        ok = co_await send_and_print(
            peer,
            agentic::ReadSymbolParams{.name = name, .path = path, .line = line});
    } else if(opts.method == "documentSymbols") {
        ok = co_await send_and_print(peer, agentic::DocumentSymbolsParams{.path = opts.path});
    } else if(opts.method == "callGraph") {
        auto name = opts.name.empty() ? std::nullopt : std::optional(opts.name);
        auto path = opts.path.empty() ? std::nullopt : std::optional(opts.path);
        auto line = opts.line > 0 ? std::optional(opts.line) : std::nullopt;
        auto dir = opts.direction.empty() ? std::nullopt : std::optional(opts.direction);
        ok = co_await send_and_print(peer,
                                     agentic::CallGraphParams{
                                         .name = name,
                                         .path = path,
                                         .line = line,
                                         .direction = dir,
                                     });
    } else if(opts.method == "typeHierarchy") {
        auto name = opts.name.empty() ? std::nullopt : std::optional(opts.name);
        auto path = opts.path.empty() ? std::nullopt : std::optional(opts.path);
        auto line = opts.line > 0 ? std::optional(opts.line) : std::nullopt;
        auto dir = opts.direction.empty() ? std::nullopt : std::optional(opts.direction);
        ok = co_await send_and_print(peer,
                                     agentic::TypeHierarchyParams{
                                         .name = name,
                                         .path = path,
                                         .line = line,
                                         .direction = dir,
                                     });
    } else if(opts.method == "fileDeps") {
        auto dir = opts.direction.empty() ? std::nullopt : std::optional(opts.direction);
        ok = co_await send_and_print(peer,
                                     agentic::FileDepsParams{.path = opts.path, .direction = dir});
    } else if(opts.method == "impactAnalysis") {
        ok = co_await send_and_print(peer, agentic::ImpactAnalysisParams{.path = opts.path});
    } else if(opts.method == "status") {
        ok = co_await send_and_print(peer, agentic::StatusParams{});
    } else if(opts.method == "shutdown") {
        peer.send_notification(agentic::ShutdownParams{});
        ok = true;
    } else {
        LOG_ERROR("unknown agentic method '{}'", opts.method);
    }

    if(ok)
        exit_code = 0;
    peer.close();
}

static kota::task<> agentic_client(int& exit_code,
                                   std::unique_ptr<kota::ipc::JsonPeer>& peer_out,
                                   const AgenticQueryOptions& opts) {
    auto& loop = kota::event_loop::current();
    auto transport = co_await kota::ipc::StreamTransport::connect_tcp(opts.host, opts.port, loop);
    if(!transport) {
        LOG_ERROR("failed to connect to {}:{}", opts.host, opts.port);
        co_return;
    }

    peer_out = std::make_unique<kota::ipc::JsonPeer>(loop, std::move(*transport));
    co_await kota::when_all(peer_out->run(), agentic_request(*peer_out, exit_code, opts));
}

int run_agentic_mode(const AgenticQueryOptions& opts) {
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

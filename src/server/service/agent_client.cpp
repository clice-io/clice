#include "server/service/agent_client.h"

#include <format>
#include <string>
#include <vector>

#include "server/protocol/agentic.h"
#include "server/service/master_server.h"

namespace clice {

using kota::ipc::RequestResult;
using RequestContext = kota::ipc::JsonPeer::RequestContext;

AgentClient::AgentClient(MasterServer& server, kota::ipc::JsonPeer& peer) :
    server(server), peer(peer) {
    using namespace agentic;

    peer.on_request(
        [this](RequestContext&,
               const CompileCommandParams& params) -> RequestResult<CompileCommandParams> {
            std::string directory;
            std::vector<std::string> arguments;
            if(!this->server.compiler.fill_compile_args(params.path, directory, arguments)) {
                co_return kota::outcome_error(
                    kota::ipc::Error{std::format("no compile command found for {}", params.path)});
            }

            co_return CompileCommandResult{
                .file = params.path,
                .directory = std::move(directory),
                .arguments = std::move(arguments),
            };
        });
}

}  // namespace clice

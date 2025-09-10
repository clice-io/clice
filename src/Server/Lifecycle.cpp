#include "Server/Server.h"

namespace clice {

async::Task<json::Value> Server::on_initialize(proto::InitializeParams params) {
    log::info("Initialize from client: {}, version: {}",
              params.clientInfo.name,
              params.clientInfo.version);

    /// FIXME: adjust position encoding.
    kind = PositionEncodingKind::UTF16;
    workspace = mapping.to_path(([&] -> std::string {
        if(params.workspaceFolders && !params.workspaceFolders->empty()) {
            return params.workspaceFolders->front().uri;
        }
        if(params.rootUri) {
            return *params.rootUri;
        }

        log::fatal("The client should provide one workspace folder or rootUri at least!");
    })());

    /// Initialize configuration.
    config::init(workspace);

    /// Set server options.
    opening_files.set_capability(config::server.max_active_file);

    /// Load user defined rules before load the compile commands.
    database.load_rules(config::rules);

    /// Load compile commands.json
    database.load_compile_database(config::server.compile_commands_dirs, workspace);

    /// Load cache info.
    load_cache_info();

    proto::InitializeResult result;
    auto& [info, capabilities] = result;
    info.name = "clice";
    info.version = "0.0.1";

    capabilities.positionEncoding = "utf-16";

    /// TextDocument synchronization.
    capabilities.textDocumentSync.openClose = true;
    /// FIXME: In the end, we should use `Incremental`.
    capabilities.textDocumentSync.change = proto::TextDocumentSyncKind::Full;
    capabilities.textDocumentSync.save = true;

    /// Completion
    capabilities.completionProvider.triggerCharacters = {".", "<", ">", ":", "\"", "/", "*"};
    capabilities.completionProvider.resolveProvider = false;
    capabilities.completionProvider.completionItem.labelDetailsSupport = true;

    /// Hover
    capabilities.hoverProvider = true;

    /// SignatureHelp
    capabilities.signatureHelpProvider.triggerCharacters = {"(", ")", "{", "}", "<", ">", ","};

    /// DocumentSymbol
    capabilities.documentSymbolProvider = {};

    /// DocumentLink
    capabilities.documentLinkProvider.resolveProvider = false;

    /// Formatting
    capabilities.documentFormattingProvider = true;
    capabilities.documentRangeFormattingProvider = true;

    /// FoldingRange
    capabilities.foldingRangeProvider = true;

    /// Semantic tokens
    capabilities.semanticTokensProvider.range = false;
    capabilities.semanticTokensProvider.full = true;
    for(auto name: SymbolKind::all()) {
        std::string type{name};
        type[0] = std::tolower(type[0]);
        capabilities.semanticTokensProvider.legend.tokenTypes.emplace_back(std::move(type));
    }

    /// Inlay hint
    /// FIXME: Resolve to make hint clickable.
    capabilities.inlayHintProvider.resolveProvider = false;

    co_return json::serialize(result);
}

async::Task<> Server::on_initialized(proto::InitializedParams) {
    co_return;
}

async::Task<json::Value> Server::on_shutdown(proto::ShutdownParams params) {
    co_return json::Value(nullptr);
}

async::Task<> Server::on_exit(proto::ExitParams params) {
    save_cache_info();
    async::stop();
    co_return;
}

}  // namespace clice

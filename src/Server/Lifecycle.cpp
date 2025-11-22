#include "Server/Server.h"

namespace clice {

async::Task<json::Value> Server::on_initialize(proto::InitializeParams params) {
    LOG_INFO("Initialize from client: {}, version: {}",
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

        LOG_FATAL("The client should provide one workspace folder or rootUri at least!");
    })());

    /// Initialize configuration.
    if(auto result = config.parse(workspace)) {
        LOG_INFO("Config initialized successfully: {0:4}", json::serialize(config));
    } else {
        LOG_WARN("Fail to load config, because: {0}", result.error());
        LOG_INFO("Use default config: {0:4}", json::serialize(config));
    }

    if(!config.project.logging_dir.empty()) {
        logging::file_loggger("clice", config.project.logging_dir, logging::options);
    }

    /// Set server options.
    opening_files.set_capability(config.project.max_active_file);

    /// Load compile commands.json
    for(auto& dir: config.project.compile_commands_dirs) {
        database.load_compile_database(path::join(dir, "compile_commands.json"));
    }

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

    /// FIXME: In the future, we would support work done progress.
    capabilities.declarationProvider.workDoneProgress = false;
    capabilities.definitionProvider.workDoneProgress = false;
    capabilities.referencesProvider.workDoneProgress = false;

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
    indexer.load_from_disk();
    co_await indexer.index_all();
    co_return;
}

async::Task<json::Value> Server::on_shutdown(proto::ShutdownParams params) {
    co_return json::Value(nullptr);
}

async::Task<> Server::on_exit(proto::ExitParams params) {
    save_cache_info();
    indexer.save_to_disk();
    async::stop();
    co_return;
}

}  // namespace clice

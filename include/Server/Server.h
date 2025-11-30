#pragma once

#include "Config.h"
#include "Convert.h"
#include "Indexer.h"
#include "Async/Async.h"
#include "Compiler/Command.h"
#include "Compiler/Diagnostic.h"
#include "Compiler/Preamble.h"
#include "Feature/DocumentLink.h"
#include "Protocol/Protocol.h"

namespace clice {

struct OpenFile {
    /// The file version, every edition will increase it.
    std::uint32_t version = 0;

    /// The file content.
    std::string content;

    /// We build PCH for every opened file.
    std::optional<PCHInfo> pch;
    async::Task<bool> pch_build_task;
    async::Event pch_built_event;
    std::vector<feature::DocumentLink> pch_includes;

    /// For each opened file, we would like to build an AST for it.
    std::shared_ptr<CompilationUnit> ast;
    async::Task<> ast_build_task;
    async::Lock ast_built_lock;

    /// Collect all diagnostics in the compilation.
    std::shared_ptr<std::vector<Diagnostic>> diagnostics =
        std::make_unique<std::vector<Diagnostic>>();

    /// For header with context, it may have multiple ASTs, use
    /// an chain to store them.
    std::unique_ptr<OpenFile> next;
};

/// A manager for all OpenFile with LRU cache.
class ActiveFileManager {
public:
    /// Use shared_ptr to manage the lifetime of OpenFile object in async function.
    using ActiveFile = std::shared_ptr<OpenFile>;

    /// A double-linked list to store all opened files. While the `first` field of pair (each node
    /// of list) refers to a key in `index`, the `second` field refers to the OpenFile object.
    /// In another word, the `index` holds the ownership of path and the `items` holds the
    /// ownership of OpenFile object.
    using ListContainer = std::list<std::pair<llvm::StringRef, ActiveFile>>;

    struct ActiveFileIterator : public ListContainer::const_iterator {};

    constexpr static size_t DefaultMaxActiveFileNum = 8;
    constexpr static size_t UnlimitedActiveFileNum = 512;

public:
    /// Create an ActiveFileManager with a default size.
    ActiveFileManager() : capability(DefaultMaxActiveFileNum) {}

    ActiveFileManager(const ActiveFileManager&) = delete;
    ActiveFileManager& operator=(const ActiveFileManager&) = delete;

    /// Set the maximum active file count and it will be clamped to [1, UnlimitedActiveFileNum].
    void set_capability(size_t size) {
        // Use static_cast to make MSVC happy.
        capability = std::clamp(size, static_cast<size_t>(1), UnlimitedActiveFileNum);
    }

    /// Get the maximum size of the cache.
    size_t max_size() const {
        return capability;
    }

    /// Get the current size of the cache.
    size_t size() const {
        return index.size();
    }

    /// Try get OpenFile from manager, default construct one if not exists.
    [[nodiscard]] ActiveFile& get_or_add(llvm::StringRef path);

    /// Add a OpenFile to the manager.
    ActiveFile& add(llvm::StringRef path, OpenFile file);

    [[nodiscard]] bool contains(llvm::StringRef path) const {
        return index.contains(path);
    }

    ActiveFileIterator begin() const {
        return ActiveFileIterator(items.begin());
    }

    ActiveFileIterator end() const {
        return ActiveFileIterator(items.end());
    }

private:
    ActiveFile& lru_put_impl(llvm::StringRef path, OpenFile file);

private:
    /// The maximum size of the cache.
    size_t capability;

    /// The first element is the most recently used, and the last
    /// element is the least recently used.
    /// When a file is accessed, it will be moved to the front of the list.
    /// When a new file is added, if the size exceeds the maximum size,
    /// the last element will be removed.
    ListContainer items;

    /// A map from path to the iterator of the list.
    llvm::StringMap<ListContainer::iterator> index;
};

class Server {
public:
    Server();

    using Self = Server;

    using Callback = async::Task<json::Value> (*)(Server&, json::Value);

    template <auto method>
    void register_callback(llvm::StringRef name) {
        using MF = decltype(method);
        static_assert(std::is_member_function_pointer_v<MF>, "");
        using F = member_type_t<MF>;
        using Ret = function_return_t<F>;
        using Params = std::tuple_element_t<0, function_args_t<F>>;

        Callback callback = [](Server& server, json::Value value) -> async::Task<json::Value> {
            if constexpr(std::is_same_v<Ret, async::Task<>>) {
                co_await (server.*method)(json::deserialize<Params>(value));
                co_return json::Value(nullptr);
            } else {
                co_return co_await (server.*method)(json::deserialize<Params>(value));
            }
        };

        callbacks.try_emplace(name, callback);
    }

    async::Task<> on_receive(json::Value value);

private:
    /// Send a request to the client.
    async::Task<> request(llvm::StringRef method, json::Value params);

    /// Send a notification to the client.
    async::Task<> notify(llvm::StringRef method, json::Value params);

    /// Send a response to the client.
    async::Task<> response(json::Value id, json::Value result);

    async::Task<> response(json::Value id, proto::ErrorCodes code, llvm::StringRef message = "");

    /// Send an register capability to the client.
    async::Task<> registerCapacity(llvm::StringRef id,
                                   llvm::StringRef method,
                                   json::Value registerOptions);

private:
    async::Task<json::Value> on_initialize(proto::InitializeParams params);

    async::Task<> on_initialized(proto::InitializedParams);

    async::Task<json::Value> on_shutdown(proto::ShutdownParams params);

    async::Task<> on_exit(proto::ExitParams params);

private:
    /// Load the cache info from disk.
    void load_cache_info();

    /// Save the cache info to disk.
    void save_cache_info();

    async::Task<bool> build_pch(std::string file, std::string preamble);

    async::Task<> build_ast(std::string file, std::string content);

    async::Task<std::shared_ptr<OpenFile>> add_document(std::string path, std::string content);

private:
    async::Task<> on_did_open(proto::DidOpenTextDocumentParams params);

    async::Task<> on_did_change(proto::DidChangeTextDocumentParams params);

    async::Task<> on_did_save(proto::DidSaveTextDocumentParams params);

    async::Task<> on_did_close(proto::DidCloseTextDocumentParams params);

private:
    using Result = async::Task<json::Value>;

    auto on_completion(proto::CompletionParams params) -> Result;

    auto on_hover(proto::HoverParams params) -> Result;

    auto on_signature_help(proto::SignatureHelpParams params) -> Result;

    auto on_go_to_declaration(proto::DeclarationParams params) -> Result;

    auto on_go_to_definition(proto::DefinitionParams params) -> Result;

    auto on_find_references(proto::ReferenceParams params) -> Result;

    auto on_document_symbol(proto::DocumentSymbolParams params) -> Result;

    auto on_document_link(proto::DocumentLinkParams params) -> Result;

    auto on_document_format(proto::DocumentFormattingParams params) -> Result;

    auto on_document_range_format(proto::DocumentRangeFormattingParams params) -> Result;

    auto on_folding_range(proto::FoldingRangeParams params) -> Result;

    auto on_semantic_token(proto::SemanticTokensParams params) -> Result;

    auto on_inlay_hint(proto::InlayHintParams params) -> Result;

private:
    /// The current request id.
    std::uint32_t server_request_id = 0;
    std::uint32_t client_request_id = 0;

    /// All registered LSP callbacks.
    llvm::StringMap<Callback> callbacks;

    PositionEncodingKind kind = PositionEncodingKind::UTF16;

    std::string workspace;

    /// The compilation database.
    CompilationDatabase database;

    /// All opening files.
    ActiveFileManager opening_files;

    PathMapping mapping;

    config::Config config;

    Indexer indexer;
};

}  // namespace clice

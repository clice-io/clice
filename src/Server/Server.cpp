#include "Server/Server.h"

#include "Compiler/CompilationUnit.h"
#include "Protocol/Basic.h"
#include "Support/FileSystem.h"
#include "Support/Logging.h"

#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/ASTMatchers/ASTMatchers.h"
#include "clang/ASTMatchers/ASTMatchersInternal.h"
#include "clang/ASTMatchers/ASTMatchersMacros.h"
#include <clang/AST/Decl.h>

namespace clice {

ActiveFileManager::ActiveFile& ActiveFileManager::lru_put_impl(llvm::StringRef path,
                                                               OpenFile file) {
    /// If the file is not in the chain, create a new OpenFile.
    if(items.size() >= capability) {
        /// If the size exceeds the maximum size, remove the last element.
        index.erase(items.back().first);
        items.pop_back();
    }
    items.emplace_front(path, std::make_shared<OpenFile>(std::move(file)));

    // fix the ownership of the StringRef of the path.
    auto [added, _] = index.insert({path, items.begin()});
    items.front().first = added->getKey();

    return items.front().second;
}

ActiveFileManager::ActiveFile& ActiveFileManager::get_or_add(llvm::StringRef path) {
    auto iter = index.find(path);
    if(iter == index.end()) {
        return lru_put_impl(path, OpenFile{});
    }

    // If the file is in the chain, move it to the front.
    items.splice(items.begin(), items, iter->second);
    return iter->second->second;
}

ActiveFileManager::ActiveFile& ActiveFileManager::add(llvm::StringRef path, OpenFile file) {
    auto iter = index.find(path);
    if(iter == index.end()) {
        return lru_put_impl(path, std::move(file));
    }
    iter->second->second = std::make_shared<OpenFile>(std::move(file));

    // If the file is in the chain, move it to the front.
    items.splice(items.begin(), items, iter->second);
    return iter->second->second;
}

async::Task<> Server::request(llvm::StringRef method, json::Value params) {
    co_await async::net::write(json::Object{
        {"jsonrpc", "2.0"                 },
        {"id",      server_request_id += 1},
        {"method",  method                },
        {"params",  std::move(params)     },
    });
}

async::Task<> Server::notify(llvm::StringRef method, json::Value params) {
    co_await async::net::write(json::Object{
        {"jsonrpc", "2.0"            },
        {"method",  method           },
        {"params",  std::move(params)},
    });
}

async::Task<> Server::response(json::Value id, json::Value result) {
    co_await async::net::write(json::Object{
        {"jsonrpc", "2.0"            },
        {"id",      std::move(id)    },
        {"result",  std::move(result)},
    });
}

async::Task<> Server::response(json::Value id, proto::ErrorCodes code, llvm::StringRef message) {
    json::Object error{
        {"code",    static_cast<int>(code)},
        {"message", message               },
    };

    co_await async::net::write(json::Object{
        {"jsonrpc", "2.0"           },
        {"id",      std::move(id)   },
        {"error",   std::move(error)},
    });
}

async::Task<> Server::registerCapacity(llvm::StringRef id,
                                       llvm::StringRef method,
                                       json::Value registerOptions) {
    co_await request("client/registerCapability",
                     json::Object{
                         {"registrations",
                          json::Array{json::Object{
                              {"id", id},
                              {"method", method},
                              {"registerOptions", std::move(registerOptions)},
                          }}},
    });
}

Server::Server() : indexer(database, config, kind) {
    register_callback<&Server::on_initialize>("initialize");
    register_callback<&Server::on_initialized>("initialized");
    register_callback<&Server::on_shutdown>("shutdown");
    register_callback<&Server::on_exit>("exit");

    register_callback<&Server::on_execute_command>("workspace/executeCommand");

    register_callback<&Server::on_did_open>("textDocument/didOpen");
    register_callback<&Server::on_did_change>("textDocument/didChange");
    register_callback<&Server::on_did_save>("textDocument/didSave");
    register_callback<&Server::on_did_close>("textDocument/didClose");

    register_callback<&Server::on_completion>("textDocument/completion");
    register_callback<&Server::on_hover>("textDocument/hover");
    register_callback<&Server::on_signature_help>("textDocument/signatureHelp");
    register_callback<&Server::on_go_to_declaration>("textDocument/declaration");
    register_callback<&Server::on_go_to_definition>("textDocument/definition");
    register_callback<&Server::on_find_references>("textDocument/references");
    register_callback<&Server::on_document_symbol>("textDocument/documentSymbol");
    register_callback<&Server::on_document_link>("textDocument/documentLink");
    register_callback<&Server::on_document_format>("textDocument/formatting");
    register_callback<&Server::on_document_range_format>("textDocument/rangeFormatting");
    register_callback<&Server::on_folding_range>("textDocument/foldingRange");
    register_callback<&Server::on_semantic_token>("textDocument/semanticTokens/full");
    register_callback<&Server::on_inlay_hint>("textDocument/inlayHint");
}

async::Task<> Server::on_receive(json::Value value) {
    auto object = value.getAsObject();
    if(!object) [[unlikely]] {
        LOG_FATAL("Invalid LSP message, not an object: {}", value);
    }

    /// If the json object has an `id`, it's a request,
    /// which needs a response. Otherwise, it's a notification.
    auto id = object->get("id");

    llvm::StringRef method;
    if(auto result = object->getString("method")) {
        method = *result;
    } else [[unlikely]] {
        LOG_WARN("Invalid LSP message, method not found: {}", value);
        if(id) {
            co_await response(std::move(*id),
                              proto::ErrorCodes::InvalidRequest,
                              "Method not found");
        }
        co_return;
    }

    json::Value params = json::Value(nullptr);
    if(auto result = object->get("params")) {
        params = std::move(*result);
    }

    /// Handle request and notification separately.
    auto it = callbacks.find(method);
    if(it == callbacks.end()) {
        LOG_INFO("Ignore unhandled method: {}", method);
        co_return;
    }

    if(id) {
        auto current_id = client_request_id++;
        auto start_time = std::chrono::steady_clock::now();

        LOG_INFO("<-- Handling request: {}({})", method, current_id);
        auto result = co_await it->second(*this, std::move(params));
        co_await response(std::move(*id), std::move(result));

        auto end_time = std::chrono::steady_clock::now();
        auto duration =
            std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
        LOG_INFO("--> Handled request: {}({}) {}ms", method, current_id, duration.count());
    } else {
        auto start_time = std::chrono::steady_clock::now();
        LOG_INFO("<-- Handling notification: {}", method);

        auto result = co_await it->second(*this, std::move(params));

        auto end_time = std::chrono::steady_clock::now();
        auto duration =
            std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
        LOG_INFO("--> Handled notification: {} {}ms", method, duration.count());
    }

    co_return;
}

/// Matches named declarations with a specific name.
///
/// See \c hasName() and \c hasAnyName() in ASTMatchers.h for details.
class ContainOffsetMatcher :
    public clang::ast_matchers::internal::SingleNodeMatcherInterface<clang::FunctionDecl> {
public:
    explicit ContainOffsetMatcher(std::vector<std::pair<clang::FileID, size_t>> offsets) :
        offsets(offsets) {}

    bool matchesNode(const clang::FunctionDecl& Node) const override {
        auto& ctx = Node.getASTContext();
        auto& mgr = ctx.getSourceManager();
        const auto* body = Node.getBody();
        if(!body) {
            return false;
        }
        auto location = body->getBeginLoc();
        auto end_location = body->getEndLoc();
        auto [begin_fid, begin_offset] = mgr.getDecomposedLoc(location);
        auto [end_fid, end_offset] = mgr.getDecomposedLoc(end_location);
        if(begin_fid.isInvalid() || end_fid.isInvalid() || begin_fid != end_fid) {
            return false;
        }

        for(const auto& [expected_fid, expected_offset]: offsets) {
            if(expected_fid != begin_fid) {
                continue;
            }
            if(begin_offset <= expected_offset && expected_offset < end_offset) [[unlikely]] {
                return true;
            }
        }
        return false;
    }

    clang::FileID fid;
    std::vector<std::pair<clang::FileID, size_t>> offsets;
};

inline clang::ast_matchers::internal::Matcher<clang::FunctionDecl>
    containOffset(std::vector<std::pair<clang::FileID, size_t>> offsets) {
    return clang::ast_matchers::internal::Matcher<clang::FunctionDecl>(
        new ContainOffsetMatcher(offsets));
}

/// A reference site is a function declaration and its content.
struct CallSite {
    /// The name of the caller function of the callee.
    std::string name;
    /// The location of the caller function of the callee.
    proto::Location location;
    /// The content of the caller function of the callee.
    std::string content;
};

/// The result of the call graph command.
struct CallGraphResult {
    /// The signature of the symbol.
    std::string signature;
    /// The content of the symbol.
    std::string content;
    /// The locations of the symbol declarations.
    std::vector<proto::Location> locations;
    /// The call sites of the symbol.
    std::vector<CallSite> callSites;
};

async::Task<json::Value> Server::on_execute_command(proto::ExecuteCommandParams params) {
    auto& command = params.command;
    auto& arguments = params.arguments;

    if(command == "workspace/constructionInfo") {
        proto::TextDocumentParams identifier =
            json::deserialize<proto::TextDocumentParams>(arguments[0]);
        // std::string symbol = json::deserialize<std::string>(arguments[1]);
        auto symbol_name = *arguments[1].getAsString();
        std::println(stderr,
                     "constructionInfo: {} uri: {}",
                     symbol_name.str(),
                     identifier.textDocument.uri);

        auto path = mapping.to_path(identifier.textDocument.uri);
        auto file = opening_files.get_or_add(path);

        {
            bool hasAst = false;
            {
                auto guard = co_await file->ast_built_lock.try_lock();
                hasAst = !!file->ast;
            }

            if(!hasAst) {
                // Read the content of the file.
                auto content = clice::fs::read(path);

                if(!content) {
                    LOG_ERROR("Failed to read the content of the file: {}", path);
                    co_return json::Value{};
                }

                co_await build_ast(path, *content);
            }
        }

        /// Try get the lock, the waiter on the lock will be resumed when
        /// guard is destroyed.
        auto guard = co_await file->ast_built_lock.try_lock();
        auto& ast = file->ast;
        if(!ast) {
            LOG_ERROR("AST not built for file: {}", path);
            co_return json::Value{};
        }

        auto& ast_context = ast->context();

        class FindDeclASTConsumer : public clang::ast_matchers::MatchFinder::MatchCallback {
        public:
            llvm::SmallVector<const clang::FunctionDecl*, 1> matched_decls;
            const clang::FunctionDecl* any_decl = nullptr;
            const clang::FunctionDecl* matched_def = nullptr;

            void run(const clang::ast_matchers::MatchFinder::MatchResult& result) override {
                if(auto* decl = result.Nodes.getNodeAs<clang::FunctionDecl>("func")) {
                    if(decl->getDefinition() == decl) {
                        any_decl = matched_def = decl;
                    } else {
                        if(!any_decl) {
                            any_decl = decl;
                        }
                        matched_decls.push_back(decl);
                    }
                }
            }
        };

        FindDeclASTConsumer consumer;
        {
            // match the symbol
            clang::ast_matchers::MatchFinder finder;
            clang::ast_matchers::DeclarationMatcher matcher =
                clang::ast_matchers::functionDecl(clang::ast_matchers::hasName(symbol_name))
                    .bind("func");
            finder.addMatcher(matcher, &consumer);
            finder.matchAST(ast_context);
        }

        // print the matched symbols
        std::println(stderr, "matched decls: {}", consumer.matched_decls.size());
        std::println(stderr, "matched def: {}", !!consumer.matched_def);

        if(auto* any_decl = consumer.any_decl) {
            auto location = any_decl->getLocation();
            auto [fid, offset] = ast_context.getSourceManager().getDecomposedLoc(location);
            auto file_path =
                ast_context.getSourceManager().getFileEntryForID(fid)->tryGetRealPathName();

            if(file_path.empty()) {
                LOG_ERROR("Failed to get the real path of the symbol in file: {} {}",
                          symbol_name,
                          path);
                co_return json::Value{};
            }

            std::println(stderr, "location: {} {}", file_path, offset);

            auto locations = co_await indexer.references(file_path, offset);

            std::vector<std::pair<clang::FileID, size_t>> offsets;
            llvm::StringRef content = ast->interested_content();
            auto& mgr = ast_context.getSourceManager();
            PositionConverter converter(content, kind);
            for(auto& location: locations) {
                auto path = mapping.to_path(location.uri);
                auto file_ref = mgr.getFileManager().getFileRef(path);
                if(!file_ref) {
                    LOG_ERROR("Failed to get the file ref of the location: {} {}",
                              location.uri,
                              path);
                    continue;
                }
                auto file_id = mgr.translateFile(*file_ref);
                offsets.push_back({file_id, clice::to_offset(kind, content, location.range.start)});
            }

            std::ranges::sort(offsets);

            class AllDeclASTConsumer : public clang::ast_matchers::MatchFinder::MatchCallback {
            public:
                llvm::SmallDenseSet<const clang::FunctionDecl*, 1> matched_decls;

                void run(const clang::ast_matchers::MatchFinder::MatchResult& result) override {
                    if(auto* decl = result.Nodes.getNodeAs<clang::FunctionDecl>("func")) {
                        matched_decls.insert(decl);
                    }
                }
            };

            AllDeclASTConsumer consumer;
            {
                // match the symbol
                clang::ast_matchers::MatchFinder finder;
                clang::ast_matchers::DeclarationMatcher matcher =
                    clang::ast_matchers::functionDecl(clang::ast_matchers::isDefinition(),
                                                      containOffset(offsets))
                        .bind("func");
                finder.addMatcher(matcher, &consumer);
                finder.matchAST(ast_context);
            }

            std::vector<CallSite> call_sites;
            std::vector<const clang::FunctionDecl*> matched_decls(consumer.matched_decls.begin(),
                                                                  consumer.matched_decls.end());
            std::sort(matched_decls.begin(),
                      matched_decls.end(),
                      [](const clang::FunctionDecl* a, const clang::FunctionDecl* b) {
                          return a->getLocation() < b->getLocation();
                      });
            for(auto& decl: matched_decls) {
                auto location = decl->getLocation();
                auto [fid, offset] = mgr.getDecomposedLoc(location);
                auto file_path = mgr.getFileEntryForID(fid)->tryGetRealPathName();
                auto content = mgr.getBufferOrNone(fid)->getBuffer();
                PositionConverter converter(content, kind);
                auto begin = converter.toPosition(offset);

                // getSourceRange
                auto source_range = decl->getSourceRange();
                // auto source_text = content.substr(source_range.getBegin().getOffset(),
                // source_range.getEnd().getOffset());
                auto [begin_fid, begin_offset] = mgr.getDecomposedLoc(source_range.getBegin());
                auto [end_fid, end_offset] = mgr.getDecomposedLoc(source_range.getEnd());
                // todo: why +1?
                auto func_content =
                    end_offset > begin_offset
                        ? content.substr(begin_offset, end_offset + 1 - begin_offset).str()
                        : "";

                call_sites.push_back(CallSite{
                    decl->getNameAsString(),
                    {mapping.to_uri(file_path), begin},
                    func_content,
                });
            }
            std::string signature;
            {
                // begin loc
                auto begin_loc = any_decl->getBeginLoc();
                auto [begin_fid, begin_offset] = mgr.getDecomposedLoc(begin_loc);
                // body start or end loc
                // auto body_start_loc = any_decl->getBody()->getBeginLoc();
                auto body = any_decl->getBody();
                auto end_loc = body ? body->getBeginLoc() : any_decl->getEndLoc();
                auto [end_fid, end_offset] = mgr.getDecomposedLoc(end_loc);

                if(begin_fid == end_fid && begin_offset <= end_offset) {
                    auto content = mgr.getBufferOrNone(begin_fid)->getBuffer();
                    signature =
                        content.substr(begin_offset, end_offset + (body ? 0 : 1) - begin_offset)
                            .str();
                }
            }
            std::string func_content;
            {
                auto source_range = any_decl->getSourceRange();
                auto [begin_fid, begin_offset] = mgr.getDecomposedLoc(source_range.getBegin());
                auto [end_fid, end_offset] = mgr.getDecomposedLoc(source_range.getEnd());
                func_content = content.substr(begin_offset, end_offset + 1 - begin_offset).str();
            }

            CallGraphResult result;

            result.signature = std::move(signature);
            result.content = std::move(func_content);
            result.locations = std::move(locations);
            result.callSites = std::move(call_sites);

            spdlog::details::registry::instance().flush_all();

            co_return json::serialize(result);
        }
    }

    co_return json::Value{};
}

}  // namespace clice

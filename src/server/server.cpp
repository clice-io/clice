#include "server/server.h"

#include "compile/compilation.h"
#include "feature/feature.h"
#include "support/logging.h"
#include "syntax/scan.h"

#include "support/filesystem.h"

#include "eventide/language/position.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"

namespace clice {

namespace et = eventide;
namespace protocol = eventide::language::protocol;

Server::Server(et::event_loop& loop,
               et::ipc::JsonPeer& peer,
               const Options& options)
    : loop(loop), peer(peer), options(options) {}

void Server::register_callbacks() {
    using Ctx = et::ipc::JsonPeer::RequestContext;

    peer.on_request([this](Ctx& ctx, const protocol::InitializeParams& p) {
        return on_initialize(ctx, p);
    });
    peer.on_request("shutdown", [this](Ctx& ctx, const protocol::Value& p) {
        return on_shutdown(ctx, p);
    });
    peer.on_notification("exit", [this](const protocol::Value& p) { on_exit(p); });
    peer.on_notification("initialized", [this](const protocol::Value& p) { on_initialized(p); });

    peer.on_notification([this](const protocol::DidOpenTextDocumentParams& p) { on_did_open(p); });
    peer.on_notification([this](const protocol::DidChangeTextDocumentParams& p) {
        on_did_change(p);
    });
    peer.on_notification([this](const protocol::DidSaveTextDocumentParams& p) { on_did_save(p); });
    peer.on_notification([this](const protocol::DidCloseTextDocumentParams& p) {
        on_did_close(p);
    });

    peer.on_request([this](Ctx& ctx, const protocol::HoverParams& p) {
        return on_hover(ctx, p);
    });
    peer.on_request([this](Ctx& ctx, const protocol::CompletionParams& p) {
        return on_completion(ctx, p);
    });
    peer.on_request([this](Ctx& ctx, const protocol::SignatureHelpParams& p) {
        return on_signature_help(ctx, p);
    });
    peer.on_request([this](Ctx& ctx, const protocol::DocumentFormattingParams& p) {
        return on_formatting(ctx, p);
    });
    peer.on_request([this](Ctx& ctx, const protocol::SemanticTokensParams& p) {
        return on_semantic_tokens(ctx, p);
    });
    peer.on_request([this](Ctx& ctx, const protocol::DocumentSymbolParams& p) {
        return on_document_symbols(ctx, p);
    });
    peer.on_request([this](Ctx& ctx, const protocol::FoldingRangeParams& p) {
        return on_folding_ranges(ctx, p);
    });
    peer.on_request([this](Ctx& ctx, const protocol::DocumentLinkParams& p) {
        return on_document_links(ctx, p);
    });
    peer.on_request([this](Ctx& ctx, const protocol::InlayHintParams& p) {
        return on_inlay_hints(ctx, p);
    });
}

// === LSP Lifecycle ===

RequestResult<protocol::InitializeParams>
Server::on_initialize(et::ipc::JsonPeer::RequestContext& ctx,
                      const protocol::InitializeParams& params) {
    LOG_INFO("Initialize request received");

    auto& lsp = params.lsp__initialize_params;
    auto& wf = params.workspace_folders_initialize_params;

    if(wf.workspace_folders.has_value() && wf.workspace_folders->has_value()) {
        auto& folders = **wf.workspace_folders;
        if(!folders.empty()) {
            workspace_root = uri_to_path(folders[0].uri);
        }
    } else if(lsp.root_uri.has_value()) {
        workspace_root = uri_to_path(*lsp.root_uri);
    }

    LOG_INFO("Workspace root: {}", workspace_root);

    config = Config::load(workspace_root);

    for(auto& cdb_path : config.project.compile_commands_paths) {
        llvm::SmallString<256> full_path(cdb_path);
        llvm::sys::path::append(full_path, "compile_commands.json");
        if(llvm::sys::fs::exists(full_path)) {
            cdb.load_compile_database(full_path.str());
            LOG_INFO("Loaded CDB from {}", full_path.str().str());
            break;
        }
        if(llvm::sys::fs::exists(cdb_path)) {
            cdb.load_compile_database(cdb_path);
            LOG_INFO("Loaded CDB from {}", cdb_path);
            break;
        }
    }

    if(!config.project.logging_dir.empty()) {
        llvm::sys::fs::create_directories(config.project.logging_dir);
        logging::file_loggger("clice", config.project.logging_dir, logging::options);
    }

    cache_manager.initialize(config, workspace_root);

    compile_graph.set_dispatcher([this](std::uint32_t path_id,
                                        et::cancellation_token token) -> et::task<bool> {
        auto path = path_pool.resolve(path_id);
        LOG_INFO("CompileGraph: dispatching compilation for {}", path.str());

        CommandOptions cmd_opts;
        cmd_opts.resource_dir = true;
        cmd_opts.query_toolchain = true;
        auto ctx = cdb.lookup(path, cmd_opts);

        CompilationParams params;
        params.kind = CompilationKind::Content;
        params.directory = ctx.directory.empty() ? workspace_root : ctx.directory.str();
        params.arguments = std::move(ctx.arguments);
        params.arguments_from_database = !ctx.directory.empty();

        auto pcms_map = compile_graph.get_pcms(path_id);
        for(auto& [name, pcm_path] : pcms_map) {
            params.pcms.try_emplace(name, pcm_path);
        }

        if(token.cancelled()) {
            co_return false;
        }

        auto scan_result = scan(llvm::StringRef());

        auto pcm_it = std::find_if(
            scan_result.modules.begin(), scan_result.modules.end(),
            [](auto&) { return true; });
        (void)pcm_it;

        if(!scan_result.module_name.empty()) {
            PCMInfo pcm_info;
            pcm_info.name = scan_result.module_name;
            auto pcm_path = cache_manager.pcm_path(scan_result.module_name);
            params.output_file = pcm_path;

            auto unit = compile(params, pcm_info);
            if(!pcm_info.path.empty()) {
                compile_graph.set_pcm_path(path_id, scan_result.module_name, pcm_info.path);
                co_return true;
            }
            co_return false;
        }

        auto pch_path = cache_manager.pch_path(path);
        params.output_file = pch_path;

        PCHInfo pch_info;
        auto unit = compile(params, pch_info);
        if(!pch_info.path.empty()) {
            auto preamble_bound = compute_preamble_bound(llvm::StringRef());
            compile_graph.set_pch_path(path_id, pch_info.path, preamble_bound);
            co_return true;
        }
        co_return false;
    });

    state = State::Running;

    protocol::InitializeResult result;
    result.server_info = protocol::ServerInfo{.name = "clice", .version = "0.1.0"};

    auto& caps = result.capabilities;
    caps.text_document_sync = protocol::TextDocumentSyncKind::Full;
    caps.hover_provider = true;
    caps.completion_provider = protocol::CompletionOptions{};
    caps.signature_help_provider = protocol::SignatureHelpOptions{};
    caps.document_formatting_provider = true;
    caps.folding_range_provider = true;
    caps.document_symbol_provider = true;
    caps.document_link_provider = protocol::DocumentLinkOptions{};
    caps.inlay_hint_provider = true;

    protocol::SemanticTokensOptions st_opts;
    st_opts.full = true;
    st_opts.legend.token_types = {
        "namespace", "type",       "class",     "enum",     "interface",
        "struct",    "typeParameter", "parameter", "variable", "property",
        "enumMember", "event",     "function",  "method",   "macro",
        "keyword",  "modifier",   "comment",   "string",   "number",
        "regexp",   "operator",   "decorator",
    };
    st_opts.legend.token_modifiers = {
        "declaration", "definition",  "readonly",    "static",
        "deprecated",  "abstract",    "async",       "modification",
        "documentation", "defaultLibrary",
    };
    caps.semantic_tokens_provider = std::move(st_opts);

    co_return result;
}

et::task<protocol::null, et::ipc::protocol::Error>
Server::on_shutdown(et::ipc::JsonPeer::RequestContext& ctx,
                    const protocol::Value& params) {
    LOG_INFO("Shutdown request received");
    state = State::ShuttingDown;
    co_return nullptr;
}

void Server::on_exit(const protocol::Value& params) {
    LOG_INFO("Exit notification received");
    peer.close_output();
    loop.stop();
}

void Server::on_initialized(const protocol::Value& params) {
    LOG_INFO("Client initialized");
}

// === Document Sync ===

void Server::on_did_open(const protocol::DidOpenTextDocumentParams& params) {
    auto& td = params.text_document;
    auto path = uri_to_path(td.uri);

    LOG_INFO("didOpen: {}", path);

    auto path_id = path_pool.intern(path);

    DocumentState doc;
    doc.path_id = path_id;
    doc.uri = td.uri;
    doc.path = path;
    doc.version = td.version;
    doc.text = td.text;
    doc.generation = 0;
    doc.build_requested = true;
    doc.build_complete = std::make_unique<et::event>(false);

    documents.try_emplace(td.uri, std::move(doc));

    auto* doc_ptr = find_document(td.uri);
    if(doc_ptr) {
        scan_dependencies(*doc_ptr);
    }

    schedule_build(td.uri);
}

void Server::on_did_change(const protocol::DidChangeTextDocumentParams& params) {
    auto* doc = find_document(params.text_document.uri);
    if(!doc)
        return;

    doc->version = params.text_document.version;
    doc->generation++;

    for(auto& change : params.content_changes) {
        std::visit(
            [&](auto& c) {
                doc->text = c.text;
            },
            change);
    }

    scan_dependencies(*doc);
    compile_graph.update(doc->path_id);

    doc->build_requested = true;
    schedule_build(params.text_document.uri);
}

void Server::on_did_save(const protocol::DidSaveTextDocumentParams& params) {
    auto* doc = find_document(params.text_document.uri);
    if(!doc)
        return;

    if(params.text.has_value()) {
        doc->text = *params.text;
        doc->generation++;
        scan_dependencies(*doc);
        compile_graph.update(doc->path_id);
        doc->build_requested = true;
        schedule_build(params.text_document.uri);
    }
}

void Server::on_did_close(const protocol::DidCloseTextDocumentParams& params) {
    auto uri = params.text_document.uri;
    LOG_INFO("didClose: {}", uri);

    auto* doc = find_document(uri);
    if(doc) {
        compile_graph.remove_unit(doc->path_id);
    }

    documents.erase(uri);
}

// === Feature Requests ===

RequestResult<protocol::HoverParams>
Server::on_hover(et::ipc::JsonPeer::RequestContext& ctx,
                 const protocol::HoverParams& params) {
    if(state == State::ShuttingDown) {
        co_return et::outcome_error(
            et::ipc::protocol::Error(et::ipc::protocol::ErrorCode::RequestFailed,
                                     "server is shutting down"));
    }

    auto& tdp = params.text_document_position_params;

    auto* doc = find_document(tdp.text_document.uri);
    if(!doc) {
        co_return std::nullopt;
    }

    if(!doc->unit && doc->build_complete) {
        co_await doc->build_complete->wait();
        doc = find_document(tdp.text_document.uri);
        if(!doc || !doc->unit) {
            co_return std::nullopt;
        }
    } else if(!doc->unit) {
        co_return std::nullopt;
    }

    auto mapper = feature::PositionMapper(doc->text, feature::PositionEncoding::UTF16);
    auto offset = mapper.to_offset(tdp.position);

    auto result = feature::hover(*doc->unit, offset);

    if(!result) {
        auto main_fid = doc->unit->interested_file();
        auto& directives = doc->unit->directives();
        auto dir_it = directives.find(main_fid);
        if(dir_it != directives.end()) {
            for(auto& inc : dir_it->second.includes) {
                auto loc = doc->unit->expansion_location(inc.location);
                auto [fid, inc_offset] = doc->unit->decompose_location(loc);
                auto end_loc = doc->unit->expansion_location(inc.filename_range.getEnd());
                auto [_, end_off] = doc->unit->decompose_location(end_loc);
                end_off += doc->unit->token_length(end_loc);

                auto start_off = inc_offset > 0 ? inc_offset - 1 : inc_offset;
                if(fid == main_fid && offset >= start_off && offset <= end_off) {
                    std::string md;
                    auto src_path = doc->unit->file_path(main_fid);
                    md += std::string(llvm::sys::path::filename(src_path));

                    if(inc.fid.isValid()) {
                        auto header_path = doc->unit->file_path(inc.fid);
                        md += ": " + std::string(header_path);
                    }

                    protocol::MarkupContent content;
                    content.kind = protocol::MarkupKind::Markdown;
                    content.value = std::move(md);

                    protocol::Hover hover;
                    hover.contents = std::move(content);
                    result = std::move(hover);
                    break;
                }
            }
        }
    }

    co_return result;
}

RequestResult<protocol::CompletionParams>
Server::on_completion(et::ipc::JsonPeer::RequestContext& ctx,
                      const protocol::CompletionParams& params) {
    if(state == State::ShuttingDown) {
        co_return et::outcome_error(
            et::ipc::protocol::Error(et::ipc::protocol::ErrorCode::RequestFailed,
                                     "server is shutting down"));
    }

    auto& tdp = params.text_document_position_params;
    auto* doc = find_document(tdp.text_document.uri);
    if(!doc) {
        co_return protocol::CompletionList{};
    }

    if(!doc->unit && doc->build_complete) {
        co_await doc->build_complete->wait();
        doc = find_document(tdp.text_document.uri);
        if(!doc) {
            co_return protocol::CompletionList{};
        }
    }

    auto mapper = feature::PositionMapper(doc->text, feature::PositionEncoding::UTF16);
    auto offset = mapper.to_offset(tdp.position);

    auto compile_params = make_compile_params(*doc, CompilationKind::Completion);
    std::get<0>(compile_params.completion) = doc->path;
    std::get<1>(compile_params.completion) = offset;

    auto items = feature::code_complete(compile_params);

    protocol::CompletionList list;
    list.is_incomplete = false;
    list.items = std::move(items);
    co_return std::move(list);
}

RequestResult<protocol::SignatureHelpParams>
Server::on_signature_help(et::ipc::JsonPeer::RequestContext& ctx,
                          const protocol::SignatureHelpParams& params) {
    if(state == State::ShuttingDown) {
        co_return et::outcome_error(
            et::ipc::protocol::Error(et::ipc::protocol::ErrorCode::RequestFailed,
                                     "server is shutting down"));
    }

    auto& tdp = params.text_document_position_params;
    auto* doc = find_document(tdp.text_document.uri);
    if(!doc) {
        co_return std::nullopt;
    }

    if(!doc->unit && doc->build_complete) {
        co_await doc->build_complete->wait();
        doc = find_document(tdp.text_document.uri);
        if(!doc) {
            co_return std::nullopt;
        }
    }

    auto mapper = feature::PositionMapper(doc->text, feature::PositionEncoding::UTF16);
    auto offset = mapper.to_offset(tdp.position);

    auto compile_params = make_compile_params(*doc, CompilationKind::Completion);
    std::get<0>(compile_params.completion) = doc->path;
    std::get<1>(compile_params.completion) = offset;

    co_return feature::signature_help(compile_params);
}

RequestResult<protocol::DocumentFormattingParams>
Server::on_formatting(et::ipc::JsonPeer::RequestContext& ctx,
                      const protocol::DocumentFormattingParams& params) {
    auto* doc = find_document(params.text_document.uri);
    if(!doc) {
        co_return std::nullopt;
    }

    co_return feature::document_format(doc->path, doc->text, std::nullopt);
}

RequestResult<protocol::SemanticTokensParams>
Server::on_semantic_tokens(et::ipc::JsonPeer::RequestContext& ctx,
                           const protocol::SemanticTokensParams& params) {
    if(state == State::ShuttingDown) {
        co_return et::outcome_error(
            et::ipc::protocol::Error(et::ipc::protocol::ErrorCode::RequestFailed,
                                     "server is shutting down"));
    }

    auto* doc = find_document(params.text_document.uri);
    if(!doc || !doc->unit) {
        co_return std::nullopt;
    }

    co_return feature::semantic_tokens(*doc->unit);
}

RequestResult<protocol::DocumentSymbolParams>
Server::on_document_symbols(et::ipc::JsonPeer::RequestContext& ctx,
                            const protocol::DocumentSymbolParams& params) {
    auto* doc = find_document(params.text_document.uri);
    if(!doc || !doc->unit) {
        co_return nullptr;
    }

    co_return feature::document_symbols(*doc->unit);
}

RequestResult<protocol::FoldingRangeParams>
Server::on_folding_ranges(et::ipc::JsonPeer::RequestContext& ctx,
                          const protocol::FoldingRangeParams& params) {
    auto* doc = find_document(params.text_document.uri);
    if(!doc || !doc->unit) {
        co_return std::nullopt;
    }

    co_return feature::folding_ranges(*doc->unit);
}

RequestResult<protocol::DocumentLinkParams>
Server::on_document_links(et::ipc::JsonPeer::RequestContext& ctx,
                          const protocol::DocumentLinkParams& params) {
    auto* doc = find_document(params.text_document.uri);
    if(!doc || !doc->unit) {
        co_return std::nullopt;
    }

    co_return feature::document_links(*doc->unit);
}

RequestResult<protocol::InlayHintParams>
Server::on_inlay_hints(et::ipc::JsonPeer::RequestContext& ctx,
                       const protocol::InlayHintParams& params) {
    auto* doc = find_document(params.text_document.uri);
    if(!doc || !doc->unit) {
        co_return std::nullopt;
    }

    auto mapper = feature::PositionMapper(doc->text, feature::PositionEncoding::UTF16);
    auto begin = mapper.to_offset(params.range.start);
    auto end = mapper.to_offset(params.range.end);

    co_return feature::inlay_hints(*doc->unit, LocalSourceRange{begin, end});
}

// === Build Pipeline ===

et::task<> Server::run_build(std::string uri) {
    auto it = documents.find(uri);
    if(it == documents.end())
        co_return;

    auto& doc = it->second;
    doc.build_running = true;

    while(doc.build_requested) {
        doc.build_requested = false;
        auto gen = doc.generation;

        auto deps_ok = co_await compile_graph.compile_deps(doc.path_id, loop);
        it = documents.find(uri);
        if(it == documents.end())
            break;
        auto& doc2 = it->second;
        if(doc2.generation != gen)
            continue;

        if(!deps_ok) {
            LOG_WARN("Dependency compilation failed for {}", doc2.path);
        }

        auto compile_params = make_compile_params(doc2, CompilationKind::Content);
        compile_params.clang_tidy = config.project.clang_tidy;

        auto unit = compile(compile_params);

        auto it3 = documents.find(uri);
        if(it3 == documents.end())
            break;
        auto& doc3 = it3->second;
        if(doc3.generation != gen)
            continue;

        auto diags = feature::diagnostics(unit);
        doc3.unit = std::make_unique<CompilationUnit>(std::move(unit));
        publish_diagnostics(uri, diags);
    }

    auto it4 = documents.find(uri);
    if(it4 != documents.end()) {
        it4->second.build_running = false;
        if(it4->second.build_complete) {
            it4->second.build_complete->set();
        }
    }
}

void Server::schedule_build(std::string uri) {
    auto* doc = find_document(uri);
    if(!doc)
        return;

    doc->build_requested = true;
    if(doc->build_complete) {
        doc->build_complete->reset();
    }

    if(doc->build_running)
        return;

    loop.schedule(run_build(std::move(uri)));
}

// === Dependency Scanning ===

void Server::scan_dependencies(DocumentState& doc) {
    auto scan_result = scan(doc.text);

    llvm::SmallVector<std::uint32_t> include_ids;
    for(auto& inc : scan_result.includes) {
        if(!inc.path.empty() && !inc.not_found) {
            auto inc_id = path_pool.intern(inc.path);
            include_ids.push_back(inc_id);
        }
    }
    fuzzy_graph.update_file(doc.path_id, include_ids);

    resolve_module_deps(doc.path_id, scan_result);
}

void Server::resolve_module_deps(std::uint32_t path_id, const ScanResult& scan_result) {
    llvm::SmallVector<std::uint32_t> dep_ids;

    for(auto& mod_name : scan_result.modules) {
        auto files = cdb.files();
        for(auto* file : files) {
            llvm::StringRef file_path(file);
            auto file_content = llvm::MemoryBuffer::getFile(file_path);
            if(!file_content)
                continue;

            auto file_scan = scan((*file_content)->getBuffer());
            if(file_scan.module_name == mod_name && file_scan.is_interface_unit) {
                auto mod_path_id = path_pool.intern(file_path);
                dep_ids.push_back(mod_path_id);

                if(!compile_graph.has_unit(mod_path_id)) {
                    compile_graph.register_unit(mod_path_id, {});
                }
                break;
            }
        }
    }

    compile_graph.register_unit(path_id, dep_ids);
}

// === Helpers ===

std::string Server::uri_to_path(const std::string& uri) {
    std::string_view sv = uri;
    if(sv.starts_with("file:///")) {
#ifdef _WIN32
        return std::string(sv.substr(8));
#else
        return std::string(sv.substr(7));
#endif
    }
    if(sv.starts_with("file://")) {
        return std::string(sv.substr(7));
    }
    return uri;
}

DocumentState* Server::find_document(const std::string& uri) {
    auto it = documents.find(uri);
    if(it == documents.end())
        return nullptr;
    return &it->second;
}

void Server::publish_diagnostics(const std::string& uri,
                                 const std::vector<protocol::Diagnostic>& diags) {
    protocol::PublishDiagnosticsParams params;
    params.uri = uri;
    params.diagnostics = diags;
    peer.send_notification(params);
}

CompilationParams Server::make_compile_params(DocumentState& doc, CompilationKind kind) {
    CompilationParams params;
    params.kind = kind;

    CommandOptions cmd_opts;
    cmd_opts.resource_dir = true;
    cmd_opts.query_toolchain = true;
    auto ctx = cdb.lookup(doc.path, cmd_opts);

    bool from_database = !ctx.directory.empty();
    params.directory = from_database ? ctx.directory.str() : workspace_root;
    params.arguments = std::move(ctx.arguments);
    params.arguments_from_database = from_database;

    params.add_remapped_file(doc.path, doc.text);

    auto pch = compile_graph.get_pch(doc.path_id);
    if(!pch.first.empty()) {
        params.pch = pch;
    }

    auto pcms = compile_graph.get_pcms(doc.path_id);
    for(auto& [name, pcm_path] : pcms) {
        params.pcms.try_emplace(name, pcm_path);
    }

    return params;
}

// === Entry point ===

int run_pipe_mode(const Options& options) {
    if(auto result = fs::init_resource_dir(options.self_path); !result) {
        LOG_WARN("Failed to init resource dir: {}", result.error());
    }

    et::event_loop loop;

    auto transport = et::ipc::StreamTransport::open_stdio(loop);
    if(!transport) {
        LOG_ERROR("Failed to open stdio transport");
        return 1;
    }

    auto peer = et::ipc::JsonPeer(loop, std::move(*transport));

    Server server(loop, peer, options);
    server.register_callbacks();

    loop.schedule(peer.run());
    loop.run();

    return 0;
}

}  // namespace clice

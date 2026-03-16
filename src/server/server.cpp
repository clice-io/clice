#include "server/server.h"

#include "compile/compilation.h"
#include "feature/feature.h"
#include "index/tu_index.h"
#include "support/logging.h"
#include "syntax/scan.h"

#include "support/filesystem.h"

#include "eventide/language/position.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
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

    // Cross-file features
    peer.on_request([this](Ctx& ctx, const protocol::DefinitionParams& p) {
        return on_go_to_definition(ctx, p);
    });
    peer.on_request([this](Ctx& ctx, const protocol::ReferenceParams& p) {
        return on_find_references(ctx, p);
    });
    peer.on_request([this](Ctx& ctx, const protocol::RenameParams& p) {
        return on_rename(ctx, p);
    });
    peer.on_request([this](Ctx& ctx, const protocol::PrepareRenameParams& p) {
        return on_prepare_rename(ctx, p);
    });
    peer.on_request([this](Ctx& ctx, const protocol::WorkspaceSymbolParams& p) {
        return on_workspace_symbol(ctx, p);
    });
    peer.on_request([this](Ctx& ctx, const protocol::CallHierarchyPrepareParams& p) {
        return on_prepare_call_hierarchy(ctx, p);
    });
    peer.on_request([this](Ctx& ctx, const protocol::CallHierarchyIncomingCallsParams& p) {
        return on_incoming_calls(ctx, p);
    });
    peer.on_request([this](Ctx& ctx, const protocol::CallHierarchyOutgoingCallsParams& p) {
        return on_outgoing_calls(ctx, p);
    });
    peer.on_request([this](Ctx& ctx, const protocol::TypeHierarchyPrepareParams& p) {
        return on_prepare_type_hierarchy(ctx, p);
    });
    peer.on_request([this](Ctx& ctx, const protocol::TypeHierarchySubtypesParams& p) {
        return on_subtypes(ctx, p);
    });
    peer.on_request([this](Ctx& ctx, const protocol::TypeHierarchySupertypesParams& p) {
        return on_supertypes(ctx, p);
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

        std::string content;
        for(auto& [uri, doc] : documents) {
            if(doc.path_id == path_id) {
                content = doc.text;
                break;
            }
        }

        if(content.empty()) {
            auto buf = llvm::MemoryBuffer::getFile(path);
            if(buf) {
                content = (*buf)->getBuffer().str();
            } else {
                LOG_WARN("CompileGraph: cannot read file {}", path.str());
                co_return false;
            }
        }

        auto scan_result = scan(content);

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

        params.add_remapped_file(path, content);

        if(token.cancelled()) {
            co_return false;
        }

        if(!scan_result.module_name.empty()) {
            PCMInfo pcm_info;
            pcm_info.name = scan_result.module_name;
            pcm_info.isInterfaceUnit = scan_result.is_interface_unit;
            auto pcm_out_path = cache_manager.pcm_path(scan_result.module_name);
            params.output_file = pcm_out_path;

            auto unit = compile(params, pcm_info);
            if(!pcm_info.path.empty()) {
                compile_graph.set_pcm_path(path_id, scan_result.module_name, pcm_info.path);
                LOG_INFO("CompileGraph: built PCM for module {} at {}", scan_result.module_name, pcm_info.path);
                co_return true;
            }
            LOG_WARN("CompileGraph: PCM compilation failed for {}", path.str());
            co_return false;
        }

        auto pch_out_path = cache_manager.pch_path(path);
        params.output_file = pch_out_path;
        auto preamble_bound = compute_preamble_bound(content);

        PCHInfo pch_info;
        auto unit = compile(params, pch_info);
        if(!pch_info.path.empty()) {
            compile_graph.set_pch_path(path_id, pch_info.path, preamble_bound);
            LOG_INFO("CompileGraph: built PCH at {}", pch_info.path);
            co_return true;
        }
        LOG_WARN("CompileGraph: PCH compilation failed for {}", path.str());
        co_return false;
    });

    if(options.enable_workers &&
       (options.stateless_worker_count > 0 || options.stateful_worker_count > 0)) {
        workers = std::make_unique<WorkerPool>(options, loop);
        loop.schedule(workers->start());
        LOG_INFO("WorkerPool starting ({} stateless, {} stateful)",
                 options.stateless_worker_count, options.stateful_worker_count);
    }

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
    caps.definition_provider = true;
    caps.references_provider = true;
    caps.rename_provider = protocol::RenameOptions{};
    caps.workspace_symbol_provider = true;
    caps.call_hierarchy_provider = true;
    caps.type_hierarchy_provider = true;

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

    if(workers) {
        co_await workers->stop();
        LOG_INFO("WorkerPool stopped");
    }

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

        if(use_workers()) {
            worker::DocumentCompileParams wparams;
            wparams.uri = doc2.uri;
            wparams.version = doc2.version;
            wparams.text = doc2.text;

            CommandOptions cmd_opts;
            cmd_opts.resource_dir = true;
            cmd_opts.query_toolchain = true;
            auto ctx = cdb.lookup(doc2.path, cmd_opts);
            wparams.directory = ctx.directory.empty() ? workspace_root : ctx.directory.str();
            for(auto* arg : ctx.arguments) {
                wparams.arguments.emplace_back(arg);
            }

            auto pch = compile_graph.get_pch(doc2.path_id);
            wparams.pch = pch;

            auto pcms = compile_graph.get_pcms(doc2.path_id);
            for(auto& [name, pcm_path] : pcms) {
                wparams.pcms.emplace_back(name.str(), pcm_path);
            }
            wparams.clang_tidy = config.project.clang_tidy;

            auto result = co_await workers->send_stateful(doc2.path_id, wparams);
            auto it3 = documents.find(uri);
            if(it3 == documents.end())
                break;
            auto& doc3 = it3->second;
            if(doc3.generation != gen)
                continue;

            if(result.has_value()) {
                (void)result.value();
            }

            if(doc3.build_complete) {
                doc3.build_complete->set();
            }
        } else {
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

        if(doc3.unit) {
            update_index(doc3);
        }
        }
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
        auto cache_it = module_name_cache.find(mod_name);
        if(cache_it != module_name_cache.end()) {
            dep_ids.push_back(cache_it->second);
            if(!compile_graph.has_unit(cache_it->second)) {
                compile_graph.register_unit(cache_it->second, {});
            }
            continue;
        }

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
                module_name_cache[mod_name] = mod_path_id;

                if(!compile_graph.has_unit(mod_path_id)) {
                    compile_graph.register_unit(mod_path_id, {});
                }
                break;
            }
        }
    }

    compile_graph.register_unit(path_id, dep_ids);
}

// === Indexing ===

void Server::update_index(DocumentState& doc) {
    if(!doc.unit)
        return;

    auto tu_index = index::TUIndex::build(*doc.unit);
    auto path_mapping = project_index.merge(tu_index);

    auto main_path_id = project_index.path_pool.path_id(doc.path);
    merged_indices[main_path_id].merge(
        main_path_id,
        tu_index.built_at,
        tu_index.graph.locations,
        tu_index.main_file_index);

    for(auto& [fid, file_index] : tu_index.file_indices) {
        auto header_local_id = tu_index.graph.path_id(fid);
        if(header_local_id < path_mapping.size()) {
            auto header_path_id = path_mapping[header_local_id];
            merged_indices[header_path_id].merge(
                header_path_id,
                main_path_id,
                file_index);
        }
    }

    if(!tu_index.symbols.empty()) {
        auto scan_result = scan(doc.text);
        if(!scan_result.module_name.empty() && scan_result.is_interface_unit) {
            module_name_cache[scan_result.module_name] = doc.path_id;
        }
    }

    LOG_INFO("Index updated for {} ({} symbols)", doc.path, tu_index.symbols.size());
}

std::optional<index::SymbolHash> Server::symbol_at(std::uint32_t path_id,
                                                    std::uint32_t offset) {
    auto it = merged_indices.find(path_id);
    if(it == merged_indices.end())
        return std::nullopt;

    std::optional<index::SymbolHash> result;
    it->second.lookup(offset, [&](const index::Occurrence& occ) -> bool {
        result = occ.target;
        return false;
    });
    return result;
}

std::string Server::path_to_uri(llvm::StringRef path) {
    std::string uri = "file://";
    uri += path;
    return uri;
}

// === Cross-file Features ===

RequestResult<protocol::DefinitionParams>
Server::on_go_to_definition(et::ipc::JsonPeer::RequestContext& ctx,
                             const protocol::DefinitionParams& params) {
    auto& tdp = params.text_document_position_params;
    auto* doc = find_document(tdp.text_document.uri);
    if(!doc) {
        co_return nullptr;
    }

    if(!doc->unit && doc->build_complete) {
        co_await doc->build_complete->wait();
        doc = find_document(tdp.text_document.uri);
        if(!doc)
            co_return nullptr;
    }

    auto mapper = feature::PositionMapper(doc->text, feature::PositionEncoding::UTF16);
    auto offset = mapper.to_offset(tdp.position);

    auto sym = symbol_at(doc->path_id, offset);
    if(!sym) {
        co_return nullptr;
    }

    std::vector<protocol::Location> locations;

    auto sym_it = project_index.symbols.find(*sym);
    if(sym_it != project_index.symbols.end()) {
        auto& symbol = sym_it->second;
        for(auto file_id : symbol.reference_files) {
            if(file_id >= project_index.path_pool.paths.size())
                continue;

            auto file_path = project_index.path_pool.path(file_id);
            auto mi_it = merged_indices.find(file_id);
            if(mi_it == merged_indices.end())
                continue;

            mi_it->second.lookup(*sym, RelationKind::Definition,
                [&](const index::Relation& rel) -> bool {
                    auto file_uri = path_to_uri(file_path);

                    std::string content;
                    for(auto& [uri, d] : documents) {
                        auto p = project_index.path_pool.path_id(d.path);
                        if(p == file_id) {
                            content = d.text;
                            break;
                        }
                    }
                    if(content.empty()) {
                        auto buf = llvm::MemoryBuffer::getFile(file_path);
                        if(buf)
                            content = (*buf)->getBuffer().str();
                    }

                    auto def_range = rel.definition_range();
                    auto m = feature::PositionMapper(content, feature::PositionEncoding::UTF16);

                    protocol::Location loc;
                    loc.uri = file_uri;
                    loc.range.start = m.to_position(def_range.begin);
                    loc.range.end = m.to_position(def_range.end);
                    locations.push_back(std::move(loc));
                    return true;
                });
        }
    }

    if(locations.empty() && sym_it != project_index.symbols.end()) {
        for(auto file_id : sym_it->second.reference_files) {
            if(file_id >= project_index.path_pool.paths.size())
                continue;

            auto file_path = project_index.path_pool.path(file_id);
            auto mi_it = merged_indices.find(file_id);
            if(mi_it == merged_indices.end())
                continue;

            mi_it->second.lookup(*sym, RelationKind::Declaration,
                [&](const index::Relation& rel) -> bool {
                    auto file_uri = path_to_uri(file_path);

                    std::string content;
                    for(auto& [uri, d] : documents) {
                        auto p = project_index.path_pool.path_id(d.path);
                        if(p == file_id) {
                            content = d.text;
                            break;
                        }
                    }
                    if(content.empty()) {
                        auto buf = llvm::MemoryBuffer::getFile(file_path);
                        if(buf)
                            content = (*buf)->getBuffer().str();
                    }

                    auto decl_range = rel.definition_range();
                    auto m = feature::PositionMapper(content, feature::PositionEncoding::UTF16);

                    protocol::Location loc;
                    loc.uri = file_uri;
                    loc.range.start = m.to_position(decl_range.begin);
                    loc.range.end = m.to_position(decl_range.end);
                    locations.push_back(std::move(loc));
                    return true;
                });
        }
    }

    if(locations.empty()) {
        co_return nullptr;
    }

    co_return protocol::Definition(std::move(locations));
}

RequestResult<protocol::ReferenceParams>
Server::on_find_references(et::ipc::JsonPeer::RequestContext& ctx,
                            const protocol::ReferenceParams& params) {
    auto& tdp = params.text_document_position_params;
    auto* doc = find_document(tdp.text_document.uri);
    if(!doc) {
        co_return std::nullopt;
    }

    if(!doc->unit && doc->build_complete) {
        co_await doc->build_complete->wait();
        doc = find_document(tdp.text_document.uri);
        if(!doc)
            co_return std::nullopt;
    }

    auto mapper = feature::PositionMapper(doc->text, feature::PositionEncoding::UTF16);
    auto offset = mapper.to_offset(tdp.position);

    auto sym = symbol_at(doc->path_id, offset);
    if(!sym) {
        co_return std::nullopt;
    }

    std::vector<protocol::Location> locations;
    bool include_decl = params.context.include_declaration;

    auto sym_it = project_index.symbols.find(*sym);
    if(sym_it != project_index.symbols.end()) {
        for(auto file_id : sym_it->second.reference_files) {
            if(file_id >= project_index.path_pool.paths.size())
                continue;

            auto file_path = project_index.path_pool.path(file_id);
            auto mi_it = merged_indices.find(file_id);
            if(mi_it == merged_indices.end())
                continue;

            std::string content;
            for(auto& [uri, d] : documents) {
                auto p = project_index.path_pool.path_id(d.path);
                if(p == file_id) {
                    content = d.text;
                    break;
                }
            }
            if(content.empty()) {
                auto buf = llvm::MemoryBuffer::getFile(file_path);
                if(buf)
                    content = (*buf)->getBuffer().str();
            }

            auto file_uri = path_to_uri(file_path);
            auto m = feature::PositionMapper(content, feature::PositionEncoding::UTF16);

            mi_it->second.lookup(*sym, RelationKind::Reference,
                [&](const index::Relation& rel) -> bool {
                    protocol::Location loc;
                    loc.uri = file_uri;
                    loc.range.start = m.to_position(rel.range.begin);
                    loc.range.end = m.to_position(rel.range.end);
                    locations.push_back(std::move(loc));
                    return true;
                });

            if(include_decl) {
                mi_it->second.lookup(*sym, RelationKind::Declaration,
                    [&](const index::Relation& rel) -> bool {
                        auto def_range = rel.definition_range();
                        protocol::Location loc;
                        loc.uri = file_uri;
                        loc.range.start = m.to_position(def_range.begin);
                        loc.range.end = m.to_position(def_range.end);
                        locations.push_back(std::move(loc));
                        return true;
                    });
                mi_it->second.lookup(*sym, RelationKind::Definition,
                    [&](const index::Relation& rel) -> bool {
                        auto def_range = rel.definition_range();
                        protocol::Location loc;
                        loc.uri = file_uri;
                        loc.range.start = m.to_position(def_range.begin);
                        loc.range.end = m.to_position(def_range.end);
                        locations.push_back(std::move(loc));
                        return true;
                    });
            }
        }
    }

    co_return std::move(locations);
}

RequestResult<protocol::RenameParams>
Server::on_rename(et::ipc::JsonPeer::RequestContext& ctx,
                   const protocol::RenameParams& params) {
    auto& tdp = params.text_document_position_params;
    auto* doc = find_document(tdp.text_document.uri);
    if(!doc) {
        co_return std::nullopt;
    }

    if(!doc->unit && doc->build_complete) {
        co_await doc->build_complete->wait();
        doc = find_document(tdp.text_document.uri);
        if(!doc)
            co_return std::nullopt;
    }

    auto mapper = feature::PositionMapper(doc->text, feature::PositionEncoding::UTF16);
    auto offset = mapper.to_offset(tdp.position);

    auto sym = symbol_at(doc->path_id, offset);
    if(!sym) {
        co_return std::nullopt;
    }

    protocol::WorkspaceEdit edit;
    llvm::StringMap<std::vector<protocol::TextEdit>> file_edits;

    auto sym_it = project_index.symbols.find(*sym);
    if(sym_it != project_index.symbols.end()) {
        for(auto file_id : sym_it->second.reference_files) {
            if(file_id >= project_index.path_pool.paths.size())
                continue;

            auto file_path = project_index.path_pool.path(file_id);
            auto mi_it = merged_indices.find(file_id);
            if(mi_it == merged_indices.end())
                continue;

            std::string content;
            for(auto& [uri, d] : documents) {
                auto p = project_index.path_pool.path_id(d.path);
                if(p == file_id) {
                    content = d.text;
                    break;
                }
            }
            if(content.empty()) {
                auto buf = llvm::MemoryBuffer::getFile(file_path);
                if(buf)
                    content = (*buf)->getBuffer().str();
            }

            auto file_uri = path_to_uri(file_path);
            auto m = feature::PositionMapper(content, feature::PositionEncoding::UTF16);

            auto collect = [&](const index::Relation& rel) -> bool {
                protocol::TextEdit te;
                te.range.start = m.to_position(rel.range.begin);
                te.range.end = m.to_position(rel.range.end);
                te.new_text = params.new_name;
                file_edits[file_uri].push_back(std::move(te));
                return true;
            };

            mi_it->second.lookup(*sym, RelationKind::Reference, collect);
            mi_it->second.lookup(*sym, RelationKind::Declaration,
                [&](const index::Relation& rel) -> bool {
                    protocol::TextEdit te;
                    te.range.start = m.to_position(rel.range.begin);
                    te.range.end = m.to_position(rel.range.end);
                    te.new_text = params.new_name;
                    file_edits[file_uri].push_back(std::move(te));
                    return true;
                });
            mi_it->second.lookup(*sym, RelationKind::Definition,
                [&](const index::Relation& rel) -> bool {
                    protocol::TextEdit te;
                    te.range.start = m.to_position(rel.range.begin);
                    te.range.end = m.to_position(rel.range.end);
                    te.new_text = params.new_name;
                    file_edits[file_uri].push_back(std::move(te));
                    return true;
                });
        }
    }

    if(!file_edits.empty()) {
        std::map<std::string, std::vector<protocol::TextEdit>> changes;
        for(auto& [uri, edits] : file_edits) {
            changes[uri.str()] = std::move(edits);
        }
        edit.changes = std::move(changes);
    }

    co_return std::move(edit);
}

RequestResult<protocol::PrepareRenameParams>
Server::on_prepare_rename(et::ipc::JsonPeer::RequestContext& ctx,
                           const protocol::PrepareRenameParams& params) {
    auto& tdp = params.text_document_position_params;
    auto* doc = find_document(tdp.text_document.uri);
    if(!doc) {
        co_return std::nullopt;
    }

    if(!doc->unit && doc->build_complete) {
        co_await doc->build_complete->wait();
        doc = find_document(tdp.text_document.uri);
        if(!doc)
            co_return std::nullopt;
    }

    auto mapper = feature::PositionMapper(doc->text, feature::PositionEncoding::UTF16);
    auto offset = mapper.to_offset(tdp.position);

    auto sym = symbol_at(doc->path_id, offset);
    if(!sym) {
        co_return std::nullopt;
    }

    auto sym_it = project_index.symbols.find(*sym);
    if(sym_it == project_index.symbols.end()) {
        co_return std::nullopt;
    }

    protocol::Range range;
    bool found = false;

    auto mi_it = merged_indices.find(doc->path_id);
    if(mi_it != merged_indices.end()) {
        mi_it->second.lookup(offset, [&](const index::Occurrence& occ) -> bool {
            if(occ.target == *sym) {
                range.start = mapper.to_position(occ.range.begin);
                range.end = mapper.to_position(occ.range.end);
                found = true;
            }
            return false;
        });
    }

    if(!found) {
        co_return std::nullopt;
    }

    protocol::PrepareRenamePlaceholder result;
    result.range = range;
    result.placeholder = sym_it->second.name;
    co_return protocol::PrepareRenameResult(std::move(result));
}

RequestResult<protocol::WorkspaceSymbolParams>
Server::on_workspace_symbol(et::ipc::JsonPeer::RequestContext& ctx,
                             const protocol::WorkspaceSymbolParams& params) {
    auto& query = params.query;

    std::vector<protocol::WorkspaceSymbol> results;

    auto to_lsp_kind = [](SymbolKind kind) -> protocol::SymbolKind {
        switch(kind) {
            case SymbolKind::Namespace: return protocol::SymbolKind::Namespace;
            case SymbolKind::Class: return protocol::SymbolKind::Class;
            case SymbolKind::Struct: return protocol::SymbolKind::Struct;
            case SymbolKind::Enum: return protocol::SymbolKind::Enum;
            case SymbolKind::Union: return protocol::SymbolKind::Struct;
            case SymbolKind::Function: return protocol::SymbolKind::Function;
            case SymbolKind::Method: return protocol::SymbolKind::Method;
            case SymbolKind::Variable: return protocol::SymbolKind::Variable;
            case SymbolKind::Field: return protocol::SymbolKind::Field;
            case SymbolKind::EnumMember: return protocol::SymbolKind::EnumMember;
            case SymbolKind::Macro: return protocol::SymbolKind::Constant;
            case SymbolKind::Type: return protocol::SymbolKind::TypeParameter;
            case SymbolKind::Concept: return protocol::SymbolKind::Interface;
            default: return protocol::SymbolKind::Variable;
        }
    };

    for(auto& [hash, symbol] : project_index.symbols) {
        if(symbol.name.empty())
            continue;

        if(!query.empty()) {
            llvm::StringRef name_ref(symbol.name);
            llvm::StringRef query_ref(query);
            if(!name_ref.contains_insensitive(query_ref))
                continue;
        }

        for(auto file_id : symbol.reference_files) {
            if(file_id >= project_index.path_pool.paths.size())
                continue;

            auto file_path = project_index.path_pool.path(file_id);
            auto mi_it = merged_indices.find(file_id);
            if(mi_it == merged_indices.end())
                continue;

            bool found = false;
            mi_it->second.lookup(hash, RelationKind::Definition,
                [&](const index::Relation& rel) -> bool {
                    auto def_range = rel.definition_range();

                    std::string content;
                    for(auto& [uri, d] : documents) {
                        auto p = project_index.path_pool.path_id(d.path);
                        if(p == file_id) {
                            content = d.text;
                            break;
                        }
                    }
                    if(content.empty()) {
                        auto buf = llvm::MemoryBuffer::getFile(file_path);
                        if(buf)
                            content = (*buf)->getBuffer().str();
                    }

                    auto m = feature::PositionMapper(content, feature::PositionEncoding::UTF16);

                    protocol::WorkspaceSymbol ws;
                    ws.name = symbol.name;
                    ws.kind = to_lsp_kind(symbol.kind);

                    protocol::Location loc;
                    loc.uri = path_to_uri(file_path);
                    loc.range.start = m.to_position(def_range.begin);
                    loc.range.end = m.to_position(def_range.end);
                    ws.location = std::move(loc);

                    results.push_back(std::move(ws));
                    found = true;
                    return false;
                });

            if(found)
                break;
        }

        if(results.size() >= 100)
            break;
    }

    co_return std::move(results);
}

RequestResult<protocol::CallHierarchyPrepareParams>
Server::on_prepare_call_hierarchy(et::ipc::JsonPeer::RequestContext& ctx,
                                   const protocol::CallHierarchyPrepareParams& params) {
    auto& tdp = params.text_document_position_params;
    auto* doc = find_document(tdp.text_document.uri);
    if(!doc) {
        co_return std::nullopt;
    }

    if(!doc->unit && doc->build_complete) {
        co_await doc->build_complete->wait();
        doc = find_document(tdp.text_document.uri);
        if(!doc)
            co_return std::nullopt;
    }

    auto mapper = feature::PositionMapper(doc->text, feature::PositionEncoding::UTF16);
    auto offset = mapper.to_offset(tdp.position);

    auto sym = symbol_at(doc->path_id, offset);
    if(!sym) {
        co_return std::nullopt;
    }

    auto sym_it = project_index.symbols.find(*sym);
    if(sym_it == project_index.symbols.end()) {
        co_return std::nullopt;
    }

    auto& symbol = sym_it->second;
    if(symbol.kind != SymbolKind::Function && symbol.kind != SymbolKind::Method) {
        co_return std::nullopt;
    }

    std::vector<protocol::CallHierarchyItem> items;

    for(auto file_id : symbol.reference_files) {
        if(file_id >= project_index.path_pool.paths.size())
            continue;

        auto mi_it = merged_indices.find(file_id);
        if(mi_it == merged_indices.end())
            continue;

        auto file_path = project_index.path_pool.path(file_id);

        mi_it->second.lookup(*sym, RelationKind::Definition,
            [&](const index::Relation& rel) -> bool {
                auto def_range = rel.definition_range();

                std::string content;
                for(auto& [uri, d] : documents) {
                    auto p = project_index.path_pool.path_id(d.path);
                    if(p == file_id) {
                        content = d.text;
                        break;
                    }
                }
                if(content.empty()) {
                    auto buf = llvm::MemoryBuffer::getFile(file_path);
                    if(buf)
                        content = (*buf)->getBuffer().str();
                }

                auto m = feature::PositionMapper(content, feature::PositionEncoding::UTF16);

                protocol::CallHierarchyItem item;
                item.name = symbol.name;
                item.kind = symbol.kind == SymbolKind::Method
                    ? protocol::SymbolKind::Method
                    : protocol::SymbolKind::Function;
                item.uri = path_to_uri(file_path);
                item.range.start = m.to_position(def_range.begin);
                item.range.end = m.to_position(def_range.end);
                item.selection_range = item.range;
                items.push_back(std::move(item));
                return false;
            });

        if(!items.empty())
            break;
    }

    co_return std::move(items);
}

RequestResult<protocol::CallHierarchyIncomingCallsParams>
Server::on_incoming_calls(et::ipc::JsonPeer::RequestContext& ctx,
                           const protocol::CallHierarchyIncomingCallsParams& params) {
    auto& item = params.item;
    auto path = uri_to_path(item.uri);
    auto path_id = project_index.path_pool.path_id(path);

    std::string content;
    for(auto& [uri, d] : documents) {
        auto p = project_index.path_pool.path_id(d.path);
        if(p == path_id) {
            content = d.text;
            break;
        }
    }
    if(content.empty()) {
        auto buf = llvm::MemoryBuffer::getFile(path);
        if(buf)
            content = (*buf)->getBuffer().str();
    }

    auto mapper = feature::PositionMapper(content, feature::PositionEncoding::UTF16);
    auto offset = mapper.to_offset(item.selection_range.start);

    auto sym = symbol_at(path_id, offset);
    if(!sym) {
        co_return std::nullopt;
    }

    std::vector<protocol::CallHierarchyIncomingCall> results;

    auto sym_it = project_index.symbols.find(*sym);
    if(sym_it != project_index.symbols.end()) {
        for(auto file_id : sym_it->second.reference_files) {
            if(file_id >= project_index.path_pool.paths.size())
                continue;

            auto mi_it = merged_indices.find(file_id);
            if(mi_it == merged_indices.end())
                continue;

            auto file_path = project_index.path_pool.path(file_id);

            std::string file_content;
            for(auto& [uri, d] : documents) {
                auto p = project_index.path_pool.path_id(d.path);
                if(p == file_id) {
                    file_content = d.text;
                    break;
                }
            }
            if(file_content.empty()) {
                auto buf = llvm::MemoryBuffer::getFile(file_path);
                if(buf)
                    file_content = (*buf)->getBuffer().str();
            }

            auto m = feature::PositionMapper(file_content, feature::PositionEncoding::UTF16);

            mi_it->second.lookup(*sym, RelationKind::Caller,
                [&](const index::Relation& rel) -> bool {
                    auto caller_it = project_index.symbols.find(rel.target_symbol);
                    if(caller_it == project_index.symbols.end())
                        return true;

                    protocol::CallHierarchyItem caller_item;
                    caller_item.name = caller_it->second.name;
                    caller_item.kind = caller_it->second.kind == SymbolKind::Method
                        ? protocol::SymbolKind::Method
                        : protocol::SymbolKind::Function;
                    caller_item.uri = path_to_uri(file_path);

                    protocol::Range call_range;
                    call_range.start = m.to_position(rel.range.begin);
                    call_range.end = m.to_position(rel.range.end);

                    caller_item.range = call_range;
                    caller_item.selection_range = call_range;

                    protocol::CallHierarchyIncomingCall call;
                    call.from = std::move(caller_item);
                    call.from_ranges.push_back(call_range);
                    results.push_back(std::move(call));
                    return true;
                });
        }
    }

    co_return std::move(results);
}

RequestResult<protocol::CallHierarchyOutgoingCallsParams>
Server::on_outgoing_calls(et::ipc::JsonPeer::RequestContext& ctx,
                           const protocol::CallHierarchyOutgoingCallsParams& params) {
    auto& item = params.item;
    auto path = uri_to_path(item.uri);
    auto path_id = project_index.path_pool.path_id(path);

    std::string content;
    for(auto& [uri, d] : documents) {
        auto p = project_index.path_pool.path_id(d.path);
        if(p == path_id) {
            content = d.text;
            break;
        }
    }
    if(content.empty()) {
        auto buf = llvm::MemoryBuffer::getFile(path);
        if(buf)
            content = (*buf)->getBuffer().str();
    }

    auto mapper = feature::PositionMapper(content, feature::PositionEncoding::UTF16);
    auto offset = mapper.to_offset(item.selection_range.start);

    auto sym = symbol_at(path_id, offset);
    if(!sym) {
        co_return std::nullopt;
    }

    std::vector<protocol::CallHierarchyOutgoingCall> results;

    auto mi_it = merged_indices.find(path_id);
    if(mi_it != merged_indices.end()) {
        mi_it->second.lookup(*sym, RelationKind::Callee,
            [&](const index::Relation& rel) -> bool {
                auto callee_it = project_index.symbols.find(rel.target_symbol);
                if(callee_it == project_index.symbols.end())
                    return true;

                protocol::CallHierarchyItem callee_item;
                callee_item.name = callee_it->second.name;
                callee_item.kind = callee_it->second.kind == SymbolKind::Method
                    ? protocol::SymbolKind::Method
                    : protocol::SymbolKind::Function;

                for(auto file_id : callee_it->second.reference_files) {
                    if(file_id >= project_index.path_pool.paths.size())
                        continue;

                    auto callee_mi_it = merged_indices.find(file_id);
                    if(callee_mi_it == merged_indices.end())
                        continue;

                    auto file_path = project_index.path_pool.path(file_id);
                    callee_item.uri = path_to_uri(file_path);

                    callee_mi_it->second.lookup(rel.target_symbol, RelationKind::Definition,
                        [&](const index::Relation& def_rel) -> bool {
                            auto def_range = def_rel.definition_range();

                            std::string fc;
                            for(auto& [uri, d] : documents) {
                                auto p = project_index.path_pool.path_id(d.path);
                                if(p == file_id) {
                                    fc = d.text;
                                    break;
                                }
                            }
                            if(fc.empty()) {
                                auto buf = llvm::MemoryBuffer::getFile(file_path);
                                if(buf)
                                    fc = (*buf)->getBuffer().str();
                            }

                            auto m = feature::PositionMapper(fc, feature::PositionEncoding::UTF16);
                            callee_item.range.start = m.to_position(def_range.begin);
                            callee_item.range.end = m.to_position(def_range.end);
                            callee_item.selection_range = callee_item.range;
                            return false;
                        });

                    break;
                }

                protocol::Range call_range;
                call_range.start = mapper.to_position(rel.range.begin);
                call_range.end = mapper.to_position(rel.range.end);

                protocol::CallHierarchyOutgoingCall call;
                call.to = std::move(callee_item);
                call.from_ranges.push_back(call_range);
                results.push_back(std::move(call));
                return true;
            });
    }

    co_return std::move(results);
}

RequestResult<protocol::TypeHierarchyPrepareParams>
Server::on_prepare_type_hierarchy(et::ipc::JsonPeer::RequestContext& ctx,
                                    const protocol::TypeHierarchyPrepareParams& params) {
    auto& tdp = params.text_document_position_params;
    auto* doc = find_document(tdp.text_document.uri);
    if(!doc) {
        co_return std::nullopt;
    }

    if(!doc->unit && doc->build_complete) {
        co_await doc->build_complete->wait();
        doc = find_document(tdp.text_document.uri);
        if(!doc)
            co_return std::nullopt;
    }

    auto mapper = feature::PositionMapper(doc->text, feature::PositionEncoding::UTF16);
    auto offset = mapper.to_offset(tdp.position);

    auto sym = symbol_at(doc->path_id, offset);
    if(!sym) {
        co_return std::nullopt;
    }

    auto sym_it = project_index.symbols.find(*sym);
    if(sym_it == project_index.symbols.end()) {
        co_return std::nullopt;
    }

    auto& symbol = sym_it->second;
    bool is_type = symbol.kind == SymbolKind::Class ||
                   symbol.kind == SymbolKind::Struct ||
                   symbol.kind == SymbolKind::Enum ||
                   symbol.kind == SymbolKind::Union;
    if(!is_type) {
        co_return std::nullopt;
    }

    auto to_lsp_kind = [](SymbolKind kind) -> protocol::SymbolKind {
        switch(kind) {
            case SymbolKind::Class: return protocol::SymbolKind::Class;
            case SymbolKind::Struct: return protocol::SymbolKind::Struct;
            case SymbolKind::Enum: return protocol::SymbolKind::Enum;
            default: return protocol::SymbolKind::Class;
        }
    };

    std::vector<protocol::TypeHierarchyItem> items;

    for(auto file_id : symbol.reference_files) {
        if(file_id >= project_index.path_pool.paths.size())
            continue;

        auto mi_it = merged_indices.find(file_id);
        if(mi_it == merged_indices.end())
            continue;

        auto file_path = project_index.path_pool.path(file_id);

        mi_it->second.lookup(*sym, RelationKind::Definition,
            [&](const index::Relation& rel) -> bool {
                auto def_range = rel.definition_range();

                std::string content;
                for(auto& [uri, d] : documents) {
                    auto p = project_index.path_pool.path_id(d.path);
                    if(p == file_id) {
                        content = d.text;
                        break;
                    }
                }
                if(content.empty()) {
                    auto buf = llvm::MemoryBuffer::getFile(file_path);
                    if(buf)
                        content = (*buf)->getBuffer().str();
                }

                auto m = feature::PositionMapper(content, feature::PositionEncoding::UTF16);

                protocol::TypeHierarchyItem ti;
                ti.name = symbol.name;
                ti.kind = to_lsp_kind(symbol.kind);
                ti.uri = path_to_uri(file_path);
                ti.range.start = m.to_position(def_range.begin);
                ti.range.end = m.to_position(def_range.end);
                ti.selection_range = ti.range;
                items.push_back(std::move(ti));
                return false;
            });

        if(!items.empty())
            break;
    }

    co_return std::move(items);
}

RequestResult<protocol::TypeHierarchySubtypesParams>
Server::on_subtypes(et::ipc::JsonPeer::RequestContext& ctx,
                     const protocol::TypeHierarchySubtypesParams& params) {
    auto& item = params.item;
    auto path = uri_to_path(item.uri);
    auto path_id = project_index.path_pool.path_id(path);

    std::string content;
    for(auto& [uri, d] : documents) {
        auto p = project_index.path_pool.path_id(d.path);
        if(p == path_id) {
            content = d.text;
            break;
        }
    }
    if(content.empty()) {
        auto buf = llvm::MemoryBuffer::getFile(path);
        if(buf)
            content = (*buf)->getBuffer().str();
    }

    auto mapper = feature::PositionMapper(content, feature::PositionEncoding::UTF16);
    auto offset = mapper.to_offset(item.selection_range.start);

    auto sym = symbol_at(path_id, offset);
    if(!sym) {
        co_return std::nullopt;
    }

    auto to_lsp_kind = [](SymbolKind kind) -> protocol::SymbolKind {
        switch(kind) {
            case SymbolKind::Class: return protocol::SymbolKind::Class;
            case SymbolKind::Struct: return protocol::SymbolKind::Struct;
            case SymbolKind::Enum: return protocol::SymbolKind::Enum;
            default: return protocol::SymbolKind::Class;
        }
    };

    std::vector<protocol::TypeHierarchyItem> results;

    auto sym_it = project_index.symbols.find(*sym);
    if(sym_it != project_index.symbols.end()) {
        for(auto file_id : sym_it->second.reference_files) {
            if(file_id >= project_index.path_pool.paths.size())
                continue;

            auto mi_it = merged_indices.find(file_id);
            if(mi_it == merged_indices.end())
                continue;

            auto file_path = project_index.path_pool.path(file_id);

            mi_it->second.lookup(*sym, RelationKind::Derived,
                [&](const index::Relation& rel) -> bool {
                    auto derived_it = project_index.symbols.find(rel.target_symbol);
                    if(derived_it == project_index.symbols.end())
                        return true;

                    std::string fc;
                    for(auto& [uri, d] : documents) {
                        auto p = project_index.path_pool.path_id(d.path);
                        if(p == file_id) {
                            fc = d.text;
                            break;
                        }
                    }
                    if(fc.empty()) {
                        auto buf = llvm::MemoryBuffer::getFile(file_path);
                        if(buf)
                            fc = (*buf)->getBuffer().str();
                    }

                    auto m = feature::PositionMapper(fc, feature::PositionEncoding::UTF16);

                    protocol::TypeHierarchyItem ti;
                    ti.name = derived_it->second.name;
                    ti.kind = to_lsp_kind(derived_it->second.kind);
                    ti.uri = path_to_uri(file_path);
                    ti.range.start = m.to_position(rel.range.begin);
                    ti.range.end = m.to_position(rel.range.end);
                    ti.selection_range = ti.range;
                    results.push_back(std::move(ti));
                    return true;
                });
        }
    }

    co_return std::move(results);
}

RequestResult<protocol::TypeHierarchySupertypesParams>
Server::on_supertypes(et::ipc::JsonPeer::RequestContext& ctx,
                       const protocol::TypeHierarchySupertypesParams& params) {
    auto& item = params.item;
    auto path = uri_to_path(item.uri);
    auto path_id = project_index.path_pool.path_id(path);

    std::string content;
    for(auto& [uri, d] : documents) {
        auto p = project_index.path_pool.path_id(d.path);
        if(p == path_id) {
            content = d.text;
            break;
        }
    }
    if(content.empty()) {
        auto buf = llvm::MemoryBuffer::getFile(path);
        if(buf)
            content = (*buf)->getBuffer().str();
    }

    auto mapper = feature::PositionMapper(content, feature::PositionEncoding::UTF16);
    auto offset = mapper.to_offset(item.selection_range.start);

    auto sym = symbol_at(path_id, offset);
    if(!sym) {
        co_return std::nullopt;
    }

    auto to_lsp_kind = [](SymbolKind kind) -> protocol::SymbolKind {
        switch(kind) {
            case SymbolKind::Class: return protocol::SymbolKind::Class;
            case SymbolKind::Struct: return protocol::SymbolKind::Struct;
            case SymbolKind::Enum: return protocol::SymbolKind::Enum;
            default: return protocol::SymbolKind::Class;
        }
    };

    std::vector<protocol::TypeHierarchyItem> results;

    auto mi_it = merged_indices.find(path_id);
    if(mi_it != merged_indices.end()) {
        mi_it->second.lookup(*sym, RelationKind::Base,
            [&](const index::Relation& rel) -> bool {
                auto base_it = project_index.symbols.find(rel.target_symbol);
                if(base_it == project_index.symbols.end())
                    return true;

                for(auto file_id : base_it->second.reference_files) {
                    if(file_id >= project_index.path_pool.paths.size())
                        continue;

                    auto base_mi_it = merged_indices.find(file_id);
                    if(base_mi_it == merged_indices.end())
                        continue;

                    auto file_path = project_index.path_pool.path(file_id);

                    base_mi_it->second.lookup(rel.target_symbol, RelationKind::Definition,
                        [&](const index::Relation& def_rel) -> bool {
                            auto def_range = def_rel.definition_range();

                            std::string fc;
                            for(auto& [uri, d] : documents) {
                                auto p = project_index.path_pool.path_id(d.path);
                                if(p == file_id) {
                                    fc = d.text;
                                    break;
                                }
                            }
                            if(fc.empty()) {
                                auto buf = llvm::MemoryBuffer::getFile(file_path);
                                if(buf)
                                    fc = (*buf)->getBuffer().str();
                            }

                            auto m = feature::PositionMapper(fc, feature::PositionEncoding::UTF16);

                            protocol::TypeHierarchyItem ti;
                            ti.name = base_it->second.name;
                            ti.kind = to_lsp_kind(base_it->second.kind);
                            ti.uri = path_to_uri(file_path);
                            ti.range.start = m.to_position(def_range.begin);
                            ti.range.end = m.to_position(def_range.end);
                            ti.selection_range = ti.range;
                            results.push_back(std::move(ti));
                            return false;
                        });

                    break;
                }

                return true;
            });
    }

    co_return std::move(results);
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

#include "server/context_service.h"

#include <format>
#include <string>
#include <vector>

#include "command/argument_parser.h"
#include "project/configuration.h"
#include "project/hosting.h"
#include "server/ast_family.h"
#include "server/session_store.h"
#include "support/logging.h"

#include "kota/ipc/lsp/uri.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/Path.h"

namespace clice {

namespace lsp = kota::ipc::lsp;

bool indicates_missing_context(llvm::ArrayRef<protocol::Diagnostic> diagnostics) {
    constexpr static llvm::StringRef codes[] = {
        "err_unknown_typename",
        "err_undeclared_var_use",
        "err_undeclared_var_use_suggest",
        "err_pp_unterminated_conditional",
    };
    for(auto& diag: diagnostics) {
        if(diag.severity != protocol::DiagnosticSeverity::Error || !diag.code.has_value()) {
            continue;
        }
        auto* code = std::get_if<std::string>(&*diag.code);
        if(code && llvm::is_contained(codes, *code)) {
            return true;
        }
    }
    return false;
}

/// Human-readable summary of the distinguishing flags of a command.
static std::string flags_label(Project& ws, ConfigID config) {
    auto argv = ws.cdb.render_full(config);
    std::string desc;
    for(std::size_t j = 0; j < argv.size(); ++j) {
        llvm::StringRef a(argv[j]);
        if(a.starts_with("-D") || a.starts_with("-O") || a.starts_with("-std=") ||
           a.starts_with("-g")) {
            if(!desc.empty())
                desc += ' ';
            desc += argv[j];
            if((a == "-D" || a == "-O") && j + 1 < argv.size()) {
                desc += argv[++j];
            }
        }
    }
    return desc;
}

std::vector<ext::ContextItem> ContextService::contexts(Fid path_id) {
    auto& ws = project;
    auto path = ws.file_table.resolve(path_id);
    std::vector<ext::ContextItem> all_items;

    // Contexts that would produce identical compilation results are
    // collapsed: identical canonical flags mean an identical compile
    // — but only for headers CONFIRMED self-contained. A header that
    // needs includer context gets a different synthesized prefix per
    // host, and an un-trialed header may turn out the same way, so
    // every host stays a distinct context for both.
    llvm::StringSet<> seen_configs;
    bool dedup_hosts = editor.commands.header_mode(path_id) == HeaderMode::SelfContained;

    for(auto host_id: ranked_hosts(ws, path_id)) {
        auto commands = host_commands(ws, path_id, host_id);
        auto host_path = ws.file_table.resolve(host_id);
        auto host_uri_opt = lsp::URI::from_file_path(std::string(ws.file_table.display(host_id)));
        if(!host_uri_opt)
            continue;

        // A multi-configuration host contributes one context per
        // CDB entry: each configuration compiles the header under
        // different preprocessor state. Hashes are those of the command
        // the header actually compiles with — the host's, edited by the
        // rules matching either file.
        CanonicalRef edit_paths[] = {host_path, path};
        auto occurrences = ws.count_occurrences(host_id, path_id);

        for(auto& entry: commands) {
            auto applied = ws.build
                               .resolve(path_id,
                                        entry.config,
                                        CommandSource::IncludeGraph,
                                        edit_paths,
                                        host_path)
                               .config;
            auto hash = ws.cdb.entry_hash_hex(applied);
            if(dedup_hosts && !seen_configs.insert(hash).second)
                continue;

            ext::ContextItem item;
            item.label = llvm::sys::path::filename(host_path).str();
            if(commands.size() > 1) {
                auto desc = flags_label(ws, applied);
                if(!desc.empty()) {
                    item.label = std::format("{} [{}]", item.label, desc);
                }
                item.command_hash = hash;
            }
            item.description = std::string(host_path);
            item.uri = host_uri_opt->str();

            // A guard-less header can be included several times by
            // one host — each occurrence is a distinct context.
            if(occurrences > 1) {
                for(std::uint32_t n = 0; n < occurrences; ++n) {
                    auto occ_item = item;
                    occ_item.label = std::format("{} (#{})", item.label, n + 1);
                    occ_item.occurrence = n;
                    all_items.push_back(std::move(occ_item));
                }
            } else {
                all_items.push_back(std::move(item));
            }
        }
    }

    // Real entries only: lookup() would synthesize a default command
    // even for unknown files, offering a bogus context that
    // switchContext would then reject. Offered even when hosts
    // exist, so a host override can be switched back to the file's
    // own command.
    if(auto entries = ws.build.entries(path_id); !entries.empty()) {
        auto uri_opt = lsp::URI::from_file_path(std::string(ws.file_table.display(path_id)));
        for(std::size_t i = 0; uri_opt && i < entries.size(); ++i) {
            auto applied =
                ws.build.resolve(path_id, entries[i].config, CommandSource::CDBExact, path, path)
                    .config;
            auto hash = ws.cdb.entry_hash_hex(applied);
            if(!seen_configs.insert(hash).second)
                continue;

            auto desc = flags_label(ws, applied);
            ext::ContextItem item;
            item.label = desc.empty() ? std::format("config #{}", i) : desc;
            item.description = ws.cdb.config(applied).directory;
            item.uri = uri_opt->str();
            item.command_hash = std::move(hash);
            all_items.push_back(std::move(item));
        }
    }

    return all_items;
}

ext::CurrentContextResult ContextService::current_context(const Session* session,
                                                          const ext::CurrentContextParams& params) {
    ext::CurrentContextResult result;
    const Selection* choice = session ? editor.selection(session->path_id) : nullptr;
    if(choice && choice->host_path_id.valid()) {
        auto ctx_path = project.file_table.resolve(choice->host_path_id);
        auto ctx_uri_opt =
            lsp::URI::from_file_path(std::string(project.file_table.display(choice->host_path_id)));
        if(ctx_uri_opt) {
            ext::ContextItem item;
            item.label = llvm::sys::path::filename(ctx_path).str();
            if(choice->occurrence.value_or(0) > 0) {
                item.label = std::format("{} (#{})", item.label, *choice->occurrence + 1);
            }
            item.description = std::string(ctx_path);
            item.uri = ctx_uri_opt->str();
            item.occurrence = choice->occurrence;
            if(!choice->command_hash.empty()) {
                item.command_hash = choice->command_hash;
            }
            result.context = std::move(item);
        }
    } else if(choice && !choice->command_hash.empty()) {
        auto& ws = project;
        ext::ContextItem item;
        item.uri = params.uri;
        item.command_hash = choice->command_hash;
        item.label = std::format("config {}", choice->command_hash.substr(0, 8));
        auto path = ws.file_table.resolve(session->path_id);
        for(auto& entry: ws.build.entries(session->path_id)) {
            auto applied =
                ws.build
                    .resolve(session->path_id, entry.config, CommandSource::CDBExact, path, path)
                    .config;
            if(ws.cdb.entry_hash_hex(applied) == choice->command_hash) {
                auto desc = flags_label(ws, applied);
                if(!desc.empty()) {
                    item.label = std::move(desc);
                }
                item.description = ws.cdb.config(applied).directory;
                break;
            }
        }
        result.context = std::move(item);
    }
    return result;
}

kota::task<ext::SwitchContextResult>
    ContextService::switch_context(Fid path_id,
                                   Session* session,
                                   Fid context_path_id,
                                   const ext::SwitchContextParams& params) {
    auto& ws = project;
    auto path = ws.file_table.resolve(path_id);

    ext::SwitchContextResult result;

    // A choice made against an outdated listing may reference
    // contexts that no longer exist — make the client re-query.
    if(params.epoch.has_value() && *params.epoch != ws.context_epoch) {
        result.stale = true;
        co_return result;
    }

    if(!session) {
        co_return result;
    }

    // Validate that `hash` names a real candidate of `entry_file` under the
    // edits of `paths` (the host and this file for a host pin) and resolve
    // the matched candidate's base entry hash — the identity that stays
    // unique when rules collapse two applied hashes onto one value.
    auto find_command = [&](Fid entry_file,
                            llvm::ArrayRef<CanonicalRef> paths,
                            llvm::StringRef hash) -> std::optional<std::string> {
        auto entry_path = ws.file_table.resolve(entry_file);
        for(auto& entry: ws.build.commands(entry_file)) {
            auto applied =
                ws.build.resolve(entry_file, entry.config, entry.source, paths, entry_path).config;
            if(ws.cdb.entry_hash_hex(applied) == hash) {
                return ws.cdb.entry_hash_hex(entry.config);
            }
        }
        return std::nullopt;
    };

    Selection saved;
    if(context_path_id == path_id && params.command_hash.has_value()) {
        // Pin one of the file's own CDB entries.
        auto base = find_command(path_id, path, *params.command_hash);
        if(!base) {
            co_return result;
        }
        saved.command_hash = *params.command_hash;
        saved.base_hash = std::move(*base);
    } else {
        // Pin a host source for a header: it must have a compile
        // command, actually (transitively) include this header, and —
        // for multi-configuration hosts — own the pinned entry.
        if(ws.build.commands(context_path_id).empty()) {
            co_return result;
        }
        if(ws.dep_graph.find_include_chain(context_path_id, path_id).empty()) {
            co_return result;
        }
        std::optional<std::string> base;
        if(params.command_hash.has_value()) {
            CanonicalRef edit_paths[] = {ws.file_table.resolve(context_path_id), path};
            base = find_command(context_path_id, edit_paths, *params.command_hash);
            if(!base) {
                co_return result;
            }
        }
        if(params.occurrence.has_value() && *params.occurrence > 0) {
            auto count = ws.count_occurrences(context_path_id, path_id);
            if(count > 0 && *params.occurrence >= count) {
                co_return result;
            }
        }
        saved.host_path_id = context_path_id;
        saved.occurrence = params.occurrence;
        saved.command_hash = params.command_hash.value_or("");
        saved.base_hash = base.value_or("");
    }

    editor.drop_header_context(path_id);
    // The new context is a different compilation identity: supersede any
    // in-flight compile and drop the state earned under the old one. It
    // also needs its own self-containment trial — a different host can
    // change the macro environment.
    ast.switch_identity(*session);
    editor.commands.forget_self_contained(path_id);

    // The table entry is the active choice; persist it across sessions:
    // the ticket resolves once a write batch whose snapshot covers this
    // mark has committed — an already-running save that snapshotted
    // earlier cannot acknowledge it, the next one does. Failed saves pulse
    // the event without advancing the epoch; after a few such wakeups the
    // request reports failure instead of parking forever on a disk that
    // cannot take the metadata (the choice stays active in memory).
    editor.selections[path_id] = std::move(saved);
    editor.mark_dirty();
    auto& blob = editor.blob;
    auto ticket = blob.ticket;
    int failed_saves = 0;
    while(ws.request_flush && ws.index_db && !ws.index_db->read_only() &&
          blob.committed_ticket < ticket) {
        auto seen = blob.committed_ticket;
        co_await blob.committed.wait();
        if(blob.committed_ticket == seen) {
            failed_saves += 1;
            if(failed_saves >= 3) {
                co_return result;
            }
        }
    }

    result.success = true;
    co_return result;
}

ext::ListConfigurationsResult ContextService::list_configurations() const {
    ext::ListConfigurationsResult result;
    for(auto tag: project.config.configurations()) {
        result.configurations.push_back(tag.str());
    }
    result.active = project.build.active_configuration().str();
    result.selected = read_selection(project.config.project.cache_dir);
    result.default_configuration = fallback_configuration(project.config).str();
    return result;
}

ext::SwitchConfigurationResult ContextService::switch_configuration(llvm::StringRef name,
                                                                    llvm::StringRef pinned) {
    if(!declares_configuration(project.config, name)) {
        LOG_WARN("Cannot select configuration {}: no rule declares it", name);
        return {};
    }
    if(declares_configuration(project.config, pinned)) {
        LOG_WARN("Cannot select configuration {}: --configuration {} pins this session's",
                 name,
                 pinned);
        return {};
    }
    if(auto written = write_selection(project.config.project.cache_dir, name); !written) {
        LOG_WARN("Cannot persist the selected configuration {}: {}",
                 name,
                 written.error().message());
        return {};
    }
    LOG_INFO("Selected configuration {}; it becomes active at the next start", name);
    return {.success = true};
}

bool ContextService::drop_orphaned_choices(SessionStore& sessions) {
    bool dropped_saved = false;
    for(auto& [session_id, session]: sessions.sessions) {
        if(!editor.selection(session_id) || editor.holds_choice(session_id)) {
            continue;
        }
        LOG_INFO("Dropping orphaned context choice for {}: its basis no longer exists",
                 project.file_table.resolve(session_id));
        editor.drop_header_context(session_id);
        ast.switch_identity(*session);
        editor.selections.erase(session_id);
        dropped_saved = true;
    }
    return dropped_saved;
}

}  // namespace clice

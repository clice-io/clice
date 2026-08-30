#include "server/service/context_service.h"

#include <format>
#include <string>
#include <vector>

#include "command/argument_parser.h"
#include "server/service/ast_family.h"
#include "server/state/session_store.h"
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
static std::string flags_label(Workspace& ws, ConfigID config) {
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

ext::QueryContextResult ContextService::query_contexts(llvm::StringRef path,
                                                       std::uint32_t path_id,
                                                       const ext::QueryContextParams& params) {
    auto& ws = workspace;
    int offset_val = std::max(0, params.offset.value_or(0));
    constexpr int page_size = 10;

    ext::QueryContextResult result;
    std::vector<ext::ContextItem> all_items;

    // Contexts that would produce identical compilation results are
    // collapsed: identical canonical flags mean an identical compile
    // — but only for headers CONFIRMED self-contained. A header that
    // needs includer context gets a different synthesized prefix per
    // host, and an un-trialed header may turn out the same way, so
    // every host stays a distinct context for both.
    llvm::StringSet<> seen_configs;
    bool dedup_hosts = resolver.header_mode(path, path_id) == HeaderMode::SelfContained;

    auto hosts = ws.dep_graph.find_host_sources(path_id);
    for(auto host_id: ws.rank_hosts(path_id, hosts)) {
        auto host_path = ws.path_pool.resolve(host_id);
        if(!ws.cdb.has_entry(host_path))
            continue;
        auto host_uri_opt = lsp::URI::from_file_path(std::string(host_path));
        if(!host_uri_opt)
            continue;

        // A multi-configuration host contributes one context per
        // CDB entry: each configuration compiles the header under
        // different preprocessor state.
        std::vector<std::string> host_append, host_remove;
        ws.config.match_rules(host_path, host_append, host_remove);
        auto candidates = ws.cdb.candidate_entries(host_path);
        auto occurrences = ws.count_occurrences(host_id, path_id);

        for(auto& entry: candidates) {
            auto applied =
                ws.cdb.apply_rules(entry.config, {.remove = host_remove, .append = host_append});
            auto hash = ws.cdb.entry_hash_hex(applied);
            if(dedup_hosts && !seen_configs.insert(hash).second)
                continue;

            ext::ContextItem item;
            item.label = llvm::sys::path::filename(host_path).str();
            if(candidates.size() > 1) {
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
    if(ws.cdb.has_entry(path)) {
        std::vector<std::string> rule_append, rule_remove;
        ws.config.match_rules(path, rule_append, rule_remove);
        auto entries = ws.cdb.candidate_entries(path);
        auto uri_opt = lsp::URI::from_file_path(std::string(path));
        for(std::size_t i = 0; uri_opt && i < entries.size(); ++i) {
            auto applied = ws.cdb.apply_rules(entries[i].config,
                                              {.remove = rule_remove, .append = rule_append});
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

    result.epoch = ws.context_epoch;
    result.total = static_cast<int>(all_items.size());
    int end = std::min(offset_val + page_size, static_cast<int>(all_items.size()));
    for(int i = offset_val; i < end; ++i) {
        result.contexts.push_back(std::move(all_items[i]));
    }
    return result;
}

ext::CurrentContextResult ContextService::current_context(llvm::StringRef path,
                                                          const Session* session,
                                                          const ext::CurrentContextParams& params) {
    ext::CurrentContextResult result;
    const SavedContext* choice =
        session ? resolver.active_choice(ContextUse::Editor, session->path_id) : nullptr;
    if(choice && choice->host_path_id != no_path_id) {
        auto ctx_path = workspace.path_pool.resolve(choice->host_path_id);
        auto ctx_uri_opt = lsp::URI::from_file_path(std::string(ctx_path));
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
        auto& ws = workspace;
        ext::ContextItem item;
        item.uri = params.uri;
        item.command_hash = choice->command_hash;
        item.label = std::format("config {}", choice->command_hash.substr(0, 8));
        if(ws.cdb.has_entry(path)) {
            std::vector<std::string> rule_append, rule_remove;
            ws.config.match_rules(path, rule_append, rule_remove);
            for(auto& entry: ws.cdb.candidate_entries(path)) {
                auto applied = ws.cdb.apply_rules(entry.config,
                                                  {.remove = rule_remove, .append = rule_append});
                if(ws.cdb.entry_hash_hex(applied) == choice->command_hash) {
                    auto desc = flags_label(ws, applied);
                    if(!desc.empty()) {
                        item.label = std::move(desc);
                    }
                    item.description = ws.cdb.config(applied).directory;
                    break;
                }
            }
        }
        result.context = std::move(item);
    }
    return result;
}

ext::SwitchContextResult ContextService::switch_context(llvm::StringRef path,
                                                        std::uint32_t path_id,
                                                        Session* session,
                                                        llvm::StringRef context_path,
                                                        std::uint32_t context_path_id,
                                                        const ext::SwitchContextParams& params) {
    auto& ws = workspace;

    ext::SwitchContextResult result;

    // A choice made against an outdated listing may reference
    // contexts that no longer exist — make the client re-query.
    if(params.epoch.has_value() && *params.epoch != ws.context_epoch) {
        result.stale = true;
        return result;
    }

    if(!session) {
        return result;
    }

    // Validate that `hash` names a real CDB entry of `entry_path` and
    // resolve the matched candidate's base entry hash — the identity that
    // stays unique when rules collapse two applied hashes onto one value.
    auto find_command = [&](llvm::StringRef entry_path,
                            llvm::StringRef hash) -> std::optional<std::string> {
        std::vector<std::string> rule_append, rule_remove;
        ws.config.match_rules(entry_path, rule_append, rule_remove);
        for(auto& entry: ws.cdb.candidate_entries(entry_path)) {
            auto applied =
                ws.cdb.apply_rules(entry.config, {.remove = rule_remove, .append = rule_append});
            if(ws.cdb.entry_hash_hex(applied) == hash) {
                return ws.cdb.entry_hash_hex(entry.config);
            }
        }
        return std::nullopt;
    };

    SavedContext saved;
    if(context_path_id == path_id && params.command_hash.has_value()) {
        // Pin one of the file's own CDB entries.
        auto base = find_command(path, *params.command_hash);
        if(!base) {
            return result;
        }
        saved.command_hash = *params.command_hash;
        saved.base_hash = std::move(*base);
    } else {
        // Pin a host source for a header: it must have a real CDB
        // entry, actually (transitively) include this header, and —
        // for multi-configuration hosts — own the pinned entry.
        if(!ws.cdb.has_entry(context_path)) {
            return result;
        }
        if(ws.dep_graph.find_include_chain(context_path_id, path_id).empty()) {
            return result;
        }
        std::optional<std::string> base;
        if(params.command_hash.has_value()) {
            base = find_command(context_path, *params.command_hash);
            if(!base) {
                return result;
            }
        }
        if(params.occurrence.has_value() && *params.occurrence > 0) {
            auto count = ws.count_occurrences(context_path_id, path_id);
            if(count > 0 && *params.occurrence >= count) {
                return result;
            }
        }
        saved.host_path_id = context_path_id;
        saved.occurrence = params.occurrence;
        saved.command_hash = params.command_hash.value_or("");
        saved.base_hash = base.value_or("");
    }

    resolver.drop_header_context(path_id);
    // The new context is a different compilation identity: supersede any
    // in-flight compile and drop the state earned under the old one. It
    // also needs its own self-containment trial — a different host can
    // change the macro environment.
    ast.switch_identity(*session);
    resolver.forget_self_contained(path_id);

    // The table entry is the active choice; persist it across sessions.
    resolver.saved_contexts[path_id] = std::move(saved);
    ws.save_cache(resolver);

    result.success = true;
    return result;
}

bool ContextService::drop_orphaned_choices(SessionStore& sessions) {
    bool dropped_saved = false;
    for(auto& [session_id, session]: sessions.sessions) {
        auto it = resolver.saved_contexts.find(session_id);
        if(it == resolver.saved_contexts.end()) {
            continue;
        }
        auto& saved = it->second;
        auto host_id = saved.host_path_id;
        auto& occurrence = saved.occurrence;
        bool orphaned = false;
        if(host_id != no_path_id) {
            orphaned = workspace.dep_graph.find_include_chain(host_id, session_id).empty();
            // A pinned occurrence can vanish while other inclusions of the
            // header survive (the chain stays non-empty) — recount it.
            if(!orphaned && occurrence.has_value()) {
                auto count = workspace.count_occurrences(host_id, session_id);
                orphaned = count > 0 && *occurrence >= count;
            }
            // The pinned host command itself can vanish (a CDB reload
            // changed the entry's flags): same validation didOpen applies.
            if(!orphaned && !saved.command_hash.empty()) {
                orphaned = !resolver.pin_alive(workspace.path_pool.resolve(host_id), saved);
            }
        } else if(!saved.command_hash.empty()) {
            // Own-entry pin: the pinned command must still exist in the CDB.
            orphaned = !resolver.pin_alive(workspace.path_pool.resolve(session_id), saved);
        }
        if(orphaned) {
            LOG_INFO("Dropping orphaned context choice for {}: its basis no longer exists",
                     workspace.path_pool.resolve(session_id));
            resolver.drop_header_context(session_id);
            ast.switch_identity(*session);
            resolver.saved_contexts.erase(it);
            dropped_saved = true;
        }
    }
    return dropped_saved;
}

}  // namespace clice

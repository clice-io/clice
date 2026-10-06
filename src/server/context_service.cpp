#include "server/context_service.h"

#include <format>
#include <string>
#include <vector>

#include "command/argument_parser.h"
#include "feature/feature.h"
#include "project/command_resolver.h"
#include "project/configuration.h"
#include "project/hosting.h"
#include "server/ast_family.h"
#include "server/session_store.h"
#include "support/logging.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/Path.h"

namespace clice {

std::size_t missing_context_errors(llvm::ArrayRef<protocol::Diagnostic> diagnostics) {
    constexpr static llvm::StringRef codes[] = {
        "err_unknown_typename",
        "err_unknown_typename_suggest",
        "err_undeclared_var_use",
        "err_undeclared_var_use_suggest",
        "err_undeclared_use",
        "err_undeclared_use_suggest",
        "err_no_member",
        "err_no_member_suggest",
        "err_no_member_template",
        "err_no_member_template_suggest",
        "err_no_template",
        "err_no_template_suggest",
        "err_typename_nested_not_found",
        "err_unknown_nested_typename_suggest",
        "ext_implicit_function_decl_c99",
        "err_pp_hash_error",
        "err_pp_unterminated_conditional",
    };
    return llvm::count_if(diagnostics, [&](const protocol::Diagnostic& diag) {
        if(diag.severity != protocol::DiagnosticSeverity::Error || !diag.code.has_value()) {
            return false;
        }
        auto* code = std::get_if<std::string>(&*diag.code);
        return code && llvm::is_contained(codes, *code);
    });
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

/// The command `host` lends the header `path_id` under its entry `entry`,
/// edited by the rules matching either file.
static ConfigID lent_config(Project& ws, Fid path_id, Fid host, const Candidate& entry) {
    auto host_path = ws.file_table.resolve(host);
    CanonicalRef edit_paths[] = {host_path, ws.file_table.resolve(path_id)};
    return ws.build
        .resolve(path_id, entry.config, CommandSource::IncludeGraph, edit_paths, host_path)
        .config;
}

/// The listing's item for `host` lending the command `applied`: named by
/// the host, the configuration's distinguishing flags when the host has
/// several, and the place it enters the header at when it does at more
/// than one.
static ext::ContextItem host_item(Project& ws,
                                  Fid host,
                                  ConfigID applied,
                                  bool several_configs,
                                  std::optional<std::uint32_t> occurrence) {
    auto shown = ws.file_table.display(host);
    ext::ContextItem item;
    item.label = llvm::sys::path::filename(shown).str();
    if(several_configs) {
        if(auto desc = flags_label(ws, applied); !desc.empty()) {
            item.label = std::format("{} [{}]", item.label, desc);
        }
        item.command_hash = ws.cdb.entry_hash_hex(applied);
    }
    if(occurrence) {
        item.label = std::format("{} (#{})", item.label, *occurrence + 1);
        item.occurrence = occurrence;
    }
    item.description = shown;
    item.uri = feature::to_uri(shown);
    return item;
}

/// The listing's item for the file's own entry `index`, compiling as
/// `applied`.
static ext::ContextItem own_item(Project& ws, Fid path_id, std::size_t index, ConfigID applied) {
    auto desc = flags_label(ws, applied);
    ext::ContextItem item;
    item.label = desc.empty() ? std::format("config #{}", index) : desc;
    item.description =
        ws.file_table.display(CanonicalPath(Spelling::absolute(ws.cdb.config(applied).directory)));
    item.uri = feature::to_uri(ws.file_table.display(path_id));
    item.command_hash = ws.cdb.entry_hash_hex(applied);
    return item;
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
        // A multi-configuration host contributes one context per CDB
        // entry, and a host entering the header more than once one per
        // place it does: each compiles the header under different
        // preprocessor state.
        auto occurrences = count_occurrences(ws, host_id, path_id);
        if(occurrences == 0) {
            continue;
        }
        auto commands = host_commands(ws, path_id, host_id);
        for(auto& entry: commands) {
            auto applied = lent_config(ws, path_id, host_id, entry);
            if(dedup_hosts && !seen_configs.insert(ws.cdb.entry_hash_hex(applied)).second) {
                continue;
            }
            if(occurrences == 1) {
                all_items.push_back(
                    host_item(ws, host_id, applied, commands.size() > 1, std::nullopt));
                continue;
            }
            for(std::uint32_t n = 0; n < occurrences; n += 1) {
                all_items.push_back(host_item(ws, host_id, applied, commands.size() > 1, n));
            }
        }
    }

    // Real entries only: lookup() would synthesize a default command
    // even for unknown files, offering a bogus context that
    // switchContext would then reject. Offered even when hosts
    // exist, so a host override can be switched back to the file's
    // own command.
    auto entries = ws.build.entries(path_id);
    for(std::size_t i = 0; i < entries.size(); i += 1) {
        auto applied =
            ws.build.resolve(path_id, entries[i].config, CommandSource::CDBExact, path, path)
                .config;
        if(seen_configs.insert(ws.cdb.entry_hash_hex(applied)).second) {
            all_items.push_back(own_item(ws, path_id, i, applied));
        }
    }

    return all_items;
}

ext::CurrentContextResult ContextService::current_context(const Session* session) {
    ext::CurrentContextResult result;
    if(!session) {
        return result;
    }
    auto& ws = project;
    auto path_id = session->path_id;
    auto path = ws.file_table.resolve(path_id);
    const Selection* choice = editor.selection(path_id);
    result.automatic = choice == nullptr;

    auto lent =
        [&](Fid host, llvm::StringRef hash, llvm::StringRef base, std::uint32_t occurrence) {
            auto commands = host_commands(ws, path_id, host);
            if(commands.empty()) {
                return;
            }
            auto host_path = ws.file_table.resolve(host);
            CanonicalRef edit_paths[] = {host_path, path};
            auto entry =
                pick_pinned_config(ws, path_id, commands, edit_paths, host_path, hash, base);
            auto several = count_occurrences(ws, host, path_id) > 1;
            result.context = host_item(ws,
                                       host,
                                       lent_config(ws, path_id, host, entry),
                                       commands.size() > 1,
                                       several ? std::optional(occurrence) : std::nullopt);
        };
    auto own = [&](llvm::StringRef hash) {
        auto entries = ws.build.entries(path_id);
        for(std::size_t i = 0; i < entries.size(); i += 1) {
            auto applied =
                ws.build.resolve(path_id, entries[i].config, CommandSource::CDBExact, path, path)
                    .config;
            if(hash.empty() || ws.cdb.entry_hash_hex(applied) == hash) {
                result.context = own_item(ws, path_id, i, applied);
                return;
            }
        }
    };

    // The choice, else what resolve_command picks: the file's own entry,
    // then the host of its resolved context or the default one.
    if(choice && choice->host_path_id.valid()) {
        lent(choice->host_path_id,
             choice->command_hash,
             choice->base_hash,
             choice->occurrence.value_or(0));
    } else if(choice) {
        own(choice->command_hash);
    } else if(!ws.build.entries(path_id).empty()) {
        own({});
    } else if(const auto* context = editor.header_context(path_id)) {
        lent(context->host_path_id, {}, {}, context->occurrence);
    } else if(auto host = default_host(ws, path_id)) {
        lent(host->file, {}, {}, 0);
    }
    return result;
}

kota::task<ext::SwitchContextResult>
    ContextService::switch_context(Fid path_id,
                                   Session& session,
                                   Fid context_path_id,
                                   const ext::SwitchContextParams& params) {
    auto& ws = project;
    auto path = ws.file_table.resolve(path_id);
    ext::SwitchContextResult result;

    // The base entry hash of the candidate of `entry_file` the listed hash
    // names under the edits of `paths`: the identity that stays unique
    // when rules collapse two applied hashes onto one value.
    auto base_of = [&](Fid entry_file,
                       llvm::ArrayRef<CanonicalRef> paths,
                       llvm::StringRef hash) -> std::string {
        auto entry_path = ws.file_table.resolve(entry_file);
        for(auto& entry: ws.build.commands(entry_file)) {
            auto applied =
                ws.build.resolve(entry_file, entry.config, entry.source, paths, entry_path).config;
            if(ws.cdb.entry_hash_hex(applied) == hash) {
                return ws.cdb.entry_hash_hex(entry.config);
            }
        }
        std::unreachable();
    };

    Selection saved;
    if(context_path_id == path_id && params.command_hash.has_value()) {
        saved.command_hash = *params.command_hash;
        saved.base_hash = base_of(path_id, path, *params.command_hash);
    } else {
        saved.host_path_id = context_path_id;
        saved.occurrence = params.occurrence;
        if(params.command_hash.has_value()) {
            CanonicalRef edit_paths[] = {ws.file_table.resolve(context_path_id), path};
            saved.command_hash = *params.command_hash;
            saved.base_hash = base_of(context_path_id, edit_paths, *params.command_hash);
        }
    }

    editor.drop_header_context(path_id);
    // The new context is a different compilation identity: supersede any
    // in-flight compile and drop the state earned under the old one. It
    // also needs its own self-containment trial — a different host can
    // change the macro environment.
    ast.switch_identity(session);
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

void ContextService::reset_context(Session& session) {
    editor.forget_selection(session.path_id);
    ast.switch_identity(session);
    editor.commands.forget_self_contained(session.path_id);
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

module clice;

import :feature.protocol;
import :project.command_resolver;
import :project.configuration;
import :project.hosting;
import :server.ast_family;
import :server.context_service;
import :server.session_store;
import :support.logging;
import :vfs.path;

namespace clice {

/// A source action running `command` with `params`, the parameters of the
/// request it stands for.
template <typename Params>
static protocol::CodeAction source_action(std::string title,
                                          std::string_view kind,
                                          std::string_view command,
                                          const Params& params) {
    auto argument = codec::dyn::to_dyn<ipc::lsp_config>(params);
    assert(argument && "the parameters of a request encode");
    return {
        .title = title,
        .kind = protocol::CodeActionKind(kind),
        .command = protocol::Command{.title = std::move(title),
                                     .command = std::string(command),
                                     .arguments = std::vector{std::move(*argument)}},
    };
}

std::vector<protocol::CodeAction> context_actions(llvm::StringRef uri,
                                                  const ext::CurrentContextResult& current,
                                                  const ext::QueryContextResult& listing) {
    std::vector<protocol::CodeAction> actions;
    if(listing.total <= 1 && current.automatic) {
        return actions;
    }
    for(auto& item: listing.contexts) {
        if(current.context && item.uri == current.context->uri &&
           item.occurrence == current.context->occurrence &&
           item.command_hash == current.context->command_hash) {
            continue;
        }
        auto title = item.uri == uri ? std::format("Compile with the command {}", item.label)
                                     : std::format("Compile as included by {}", item.label);
        actions.push_back(source_action(std::move(title),
                                        context_action_kind,
                                        switch_context_command,
                                        ext::SwitchContextParams{.uri = uri.str(),
                                                                 .context_uri = item.uri,
                                                                 .occurrence = item.occurrence,
                                                                 .command_hash = item.command_hash,
                                                                 .epoch = listing.epoch}));
    }
    if(!current.automatic) {
        actions.push_back(source_action("Compile in the context clice picks",
                                        context_action_kind,
                                        reset_context_command,
                                        ext::ResetContextParams{.uri = uri.str()}));
    }
    return actions;
}

std::vector<protocol::CodeAction> configuration_actions(llvm::StringRef uri,
                                                        const ext::ListConfigurationsResult& list) {
    std::vector<protocol::CodeAction> actions;
    auto next = list.selected.empty() ? list.active : list.selected;
    for(auto& name: list.configurations) {
        if(name != next) {
            actions.push_back(
                source_action(std::format("Use build configuration {} from the next start", name),
                              configuration_action_kind,
                              switch_configuration_command,
                              ext::SwitchConfigurationParams{.name = name, .uri = uri.str()}));
        }
    }
    return actions;
}

/// The flags a context's label tells a command by: each -D, -O, -std and
/// -g, a separate value joined to its -D or -O.
static llvm::SmallVector<std::string> state_flags(Project& ws, CommandID command) {
    auto argv = ws.commands.render_full(command);
    llvm::SmallVector<std::string> flags;
    for(std::size_t j = 0; j < argv.size(); j += 1) {
        llvm::StringRef a(argv[j]);
        if(a.starts_with("-D") || a.starts_with("-O") || a.starts_with("-std=") ||
           a.starts_with("-g")) {
            flags.push_back(a.str());
            if((a == "-D" || a == "-O") && j + 1 < argv.size()) {
                j += 1;
                flags.back() += argv[j];
            }
        }
    }
    return flags;
}

/// What sets the command `command` apart from the others in `group`, the
/// commands the listing offers side by side: those of its flags some other
/// command lacks, or all of them when it stands alone.
static std::string flags_label(Project& ws, CommandID command, llvm::ArrayRef<CommandID> group) {
    llvm::SmallVector<llvm::SmallVector<std::string>> others;
    for(auto other: group) {
        if(other != command) {
            others.push_back(state_flags(ws, other));
        }
    }
    std::string label;
    for(auto& flag: state_flags(ws, command)) {
        if(!others.empty() &&
           ranges::all_of(others, [&](auto& flags) { return ranges::contains(flags, flag); })) {
            continue;
        }
        if(!label.empty()) {
            label += ' ';
        }
        label += flag;
    }
    return label;
}

/// The command the file's own entry `entry` compiles it as.
static CommandID own_command(Project& ws, Fid path_id, CommandID entry) {
    auto path = ws.file_table.resolve(path_id);
    return ws.build.resolve(path_id, entry, CommandSource::CDBExact, path, path).command;
}

/// The commands the file's own `entries` compile it as, one per entry.
static llvm::SmallVector<CommandID, 2> own_commands(Project& ws, Fid path_id, const auto& entries) {
    llvm::SmallVector<CommandID, 2> commands;
    for(auto& entry: entries) {
        commands.push_back(own_command(ws, path_id, entry.command));
    }
    return commands;
}

/// The commands `host` lends the header `path_id`, one per command of the
/// host's.
static llvm::SmallVector<CommandID, 2>
    lent_commands(Project& ws, Fid path_id, Fid host, llvm::ArrayRef<Candidate> commands) {
    llvm::SmallVector<CommandID, 2> lent;
    for(auto& candidate: commands) {
        lent.push_back(lent_command(ws, path_id, host, candidate.command).command);
    }
    return lent;
}

/// The listing's item for `host` lending the command `applied`, one of the
/// commands `lent` it lends the header: named by the host, the flags that
/// set the command apart when it lends several, and the place it enters
/// the header at when it does at more than one.
static ext::ContextItem host_item(Project& ws,
                                  Fid host,
                                  CommandID applied,
                                  llvm::ArrayRef<CommandID> lent,
                                  std::optional<std::uint32_t> occurrence) {
    auto shown = ws.file_table.display(host);
    ext::ContextItem item;
    item.label = path::filename(shown);
    if(lent.size() > 1) {
        if(auto desc = flags_label(ws, applied, lent); !desc.empty()) {
            item.label = std::format("{} [{}]", item.label, desc);
        }
        item.command_hash = ws.commands.command_hash_hex(applied);
    }
    if(occurrence) {
        item.label = std::format("{} (#{})", item.label, *occurrence + 1);
        item.occurrence = occurrence;
    }
    item.description = shown;
    item.uri = feature::to_uri(shown);
    return item;
}

/// The listing's item for the file's own entry `index` of `own`, the
/// commands its entries compile it as.
static ext::ContextItem
    own_item(Project& ws, Fid path_id, std::size_t index, llvm::ArrayRef<CommandID> own) {
    auto applied = own[index];
    auto desc = flags_label(ws, applied, own);
    ext::ContextItem item;
    item.label = desc.empty() ? std::format("command #{}", index) : desc;
    item.description = ws.file_table.display(
        CanonicalPath(Spelling::absolute(ws.commands.get(applied).directory)));
    item.uri = feature::to_uri(ws.file_table.display(path_id));
    item.command_hash = ws.commands.command_hash_hex(applied);
    return item;
}

std::vector<ext::ContextItem> ContextService::contexts(Fid path_id) {
    auto& ws = project;
    auto path = ws.file_table.resolve(path_id);
    std::vector<ext::ContextItem> all_items;

    // Contexts that would produce identical compilation results are
    // collapsed: identical canonical flags mean an identical compile
    // — but only for a header its trial found self-contained under the
    // command. One that needs includer context gets a different
    // synthesized prefix per host, and an un-trialed one may turn out the
    // same way, so every host stays a distinct context for both.
    llvm::StringSet<> seen_commands;
    for(auto host_id: ranked_hosts(ws, path_id)) {
        // A multi-configuration host contributes one context per CDB
        // entry, and a host entering the header more than once one per
        // place it does: each compiles the header under different
        // preprocessor state. One whose tree no run took yet is listed
        // once.
        auto occurrences = count_occurrences(ws, host_id, path_id).value_or(1);
        if(occurrences == 0) {
            continue;
        }
        auto lent = lent_commands(ws, path_id, host_id, host_commands(ws, path_id, host_id));
        for(auto applied: lent) {
            auto hash = ws.commands.command_hash(applied);
            if(self_contained(ws, {path_id, hash}, true).value_or(false) &&
               !seen_commands.insert(ws.commands.command_hash_hex(applied)).second) {
                continue;
            }
            if(occurrences == 1) {
                all_items.push_back(host_item(ws, host_id, applied, lent, std::nullopt));
                continue;
            }
            for(std::uint32_t n = 0; n < occurrences; n += 1) {
                all_items.push_back(host_item(ws, host_id, applied, lent, n));
            }
        }
    }

    // Real entries only: lookup() would synthesize a default command
    // even for unknown files, offering a bogus context that
    // switchContext would then reject. Offered even when hosts
    // exist, so a host override can be switched back to the file's
    // own command.
    auto own = own_commands(ws, path_id, ws.build.entries(path_id));
    for(std::size_t i = 0; i < own.size(); i += 1) {
        if(seen_commands.insert(ws.commands.command_hash_hex(own[i])).second) {
            all_items.push_back(own_item(ws, path_id, i, own));
        }
    }

    return all_items;
}

/// How clice/currentContext names where a command comes from.
static std::string_view source_name(CommandSource source) {
    switch(source) {
        case CommandSource::CDBExact: return "database";
        case CommandSource::IncludeGraph: return "host";
        case CommandSource::Default: return "default";
        case CommandSource::Inferred: return "inferred";
        case CommandSource::Fallback: return "fallback";
    }
    std::unreachable();
}

kota::task<ext::CurrentContextResult> ContextService::current_context(const Session* session) {
    ext::CurrentContextResult result;
    if(!session) {
        co_return result;
    }
    auto& ws = project;
    auto path_id = session->path_id;
    auto& command = result.command.emplace();
    std::vector<std::string> resolved;
    // As the document's compile does, with the tree of the host it waits
    // for: the compile in flight takes the same one.
    auto resolution = co_await trees.resolve(
        [&] { return editor.resolve_command(path_id, command.directory, resolved); });
    // The command line as its database or rule writes it, the rules
    // applied, rather than the frontend's the compile gets: the one a user
    // recognizes, and could run.
    auto file = ws.file_table.spelling(path_id).str();
    for(auto* argument: ws.commands.render_full(resolution.ref.command, file)) {
        command.arguments.emplace_back(argument);
    }
    command.source = source_name(resolution.source);
    auto from = resolution.host.valid() ? resolution.host : resolution.lender;
    if(from.valid()) {
        command.from = feature::to_uri(ws.file_table.display(from));
    }
    if(!resolution.query_failure.empty()) {
        command.query_failure = std::move(resolution.query_failure);
    }
    auto path = ws.file_table.resolve(path_id);
    const Selection* choice = editor.selection(path_id);
    result.automatic = choice == nullptr;

    auto lent =
        [&](Fid host, llvm::StringRef hash, llvm::StringRef base, std::uint32_t occurrence) {
            auto applied = host_command(ws, path_id, host, hash, base);
            if(!applied) {
                return;
            }
            auto several = count_occurrences(ws, host, path_id).value_or(1) > 1;
            result.context =
                host_item(ws,
                          host,
                          applied->command,
                          lent_commands(ws, path_id, host, host_commands(ws, path_id, host)),
                          several ? std::optional(occurrence) : std::nullopt);
        };
    auto own = [&](llvm::StringRef hash, llvm::StringRef base) {
        auto commands = ws.build.commands(path_id);
        if(commands.empty()) {
            return;
        }
        auto entry = pick_pinned_command(ws, path_id, commands, path, path, hash, base);
        auto index = ranges::find_if(commands,
                                     [&](const Candidate& candidate) {
                                         return candidate.command == entry.command;
                                     }) -
                     commands.begin();
        result.context = own_item(ws, path_id, index, own_commands(ws, path_id, commands));
    };

    // The choice, else what resolve_command picks: the file's own command
    // — listed only when it is an entry, not a rule's default — then the
    // host it borrows a command from.
    if(choice && choice->host_path_id.valid()) {
        // FIXME: reported even when the compile runs elsewhere: the chosen host
        // no longer enters the header (the default host stands in), or its
        // context fails to synthesize.
        lent(choice->host_path_id,
             choice->command_hash,
             choice->base_hash,
             choice->occurrence.value_or(0));
    } else if(choice) {
        own(choice->command_hash, choice->base_hash);
    } else if(ws.build.unit(path_id)) {
        if(!ws.build.entries(path_id).empty()) {
            own({}, {});
        }
    } else if(resolution.host.valid()) {
        lent(resolution.host, {}, {}, resolution.occurrence);
    }
    co_return result;
}

kota::task<ext::SwitchContextResult>
    ContextService::switch_context(Session& session,
                                   Fid context_path_id,
                                   const ext::SwitchContextParams& params) {
    auto& ws = project;
    auto path_id = session.path_id;
    ext::SwitchContextResult result;

    // The base command hash of the one of `commands` listed as `listed(entry)`
    // under the picked hash: the identity that stays unique when rules
    // collapse two applied hashes onto one value. The caller offered only
    // listed items.
    auto base_of = [&](llvm::ArrayRef<Candidate> commands, auto listed) -> std::string {
        for(auto& entry: commands) {
            if(ws.commands.command_hash_hex(listed(entry)) == *params.command_hash) {
                return ws.commands.command_hash_hex(entry.command);
            }
        }
        std::unreachable();
    };

    // Of the listed items only the file's own entries name the file itself,
    // each with its command hash.
    Selection saved;
    if(context_path_id == path_id) {
        saved.command_hash = *params.command_hash;
        saved.base_hash = base_of(ws.build.commands(path_id), [&](const Candidate& entry) {
            return own_command(ws, path_id, entry.command);
        });
    } else {
        saved.host_path_id = context_path_id;
        saved.occurrence = params.occurrence;
        if(params.command_hash.has_value()) {
            saved.command_hash = *params.command_hash;
            saved.base_hash =
                base_of(host_commands(ws, path_id, context_path_id), [&](const Candidate& entry) {
                    return lent_command(ws, path_id, context_path_id, entry.command).command;
                });
        }
    }

    leave_context(session);

    // The table entry is the active choice; persist it across sessions:
    // the mark is durable once a write batch whose snapshot covers its
    // number has committed — an already-running save that snapshotted
    // earlier cannot acknowledge it, the next one does. Failed saves pulse
    // the event without advancing the epoch; after a few such wakeups the
    // request reports failure instead of parking forever on a disk that
    // cannot take the metadata (the choice stays active in memory).
    editor.selections[path_id] = std::move(saved);
    editor.mark_dirty();
    auto& blob = editor.blob;
    auto seq = blob.save_seq;
    int failed_saves = 0;
    while(ws.request_flush && ws.database && !ws.database->read_only() &&
          blob.committed_seq < seq) {
        auto seen = blob.committed_seq;
        co_await blob.committed.wait();
        if(blob.committed_seq == seen) {
            failed_saves += 1;
            if(failed_saves >= 3) {
                co_return result;
            }
        }
    }

    result.success = true;
    co_return result;
}

void ContextService::leave_context(Session& session) {
    // The new context is a different compilation identity: supersede any
    // in-flight compile and drop the state earned under the old one.
    ast.switch_identity(session);
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
    auto refuse = [&](std::string reason) {
        LOG_WARN("Cannot select configuration {}: {}", name, reason);
        return ext::SwitchConfigurationResult{.reason = std::move(reason)};
    };
    if(!declares_configuration(project.config, name)) {
        return refuse(std::format("no rule declares the configuration {}", name));
    }
    if(declares_configuration(project.config, pinned)) {
        return refuse(
            std::format("the server runs with --configuration {}, which pins it", pinned));
    }
    if(auto written = write_selection(project.config.project.cache_dir, name); !written) {
        return refuse(std::format("the selection cannot be saved: {}", written.error().message()));
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
        ast.switch_identity(*session);
        editor.selections.erase(session_id);
        dropped_saved = true;
    }
    return dropped_saved;
}

}  // namespace clice

module clice;

import :index.project_index;
import :project.command_resolver;
import :project.contexts;
import :project.hosting;
import :project.project;
import :support.json;
import :support.logging;
import :syntax.dependency_graph;
import :vfs.path;

namespace clice {

namespace {

/// The flags a context's label tells a command by: each -D, -O, -std and
/// -g, a separate value joined to its -D or -O.
llvm::SmallVector<std::string> state_flags(Project& project, CommandID command) {
    auto argv = project.commands.render_full(command);
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
std::string flags_label(Project& project, CommandID command, llvm::ArrayRef<CommandID> group) {
    llvm::SmallVector<llvm::SmallVector<std::string>> others;
    for(auto other: group) {
        if(other != command) {
            others.push_back(state_flags(project, other));
        }
    }
    std::string label;
    for(auto& flag: state_flags(project, command)) {
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
CommandID own_command(Project& project, Fid file, CommandID entry) {
    auto path = project.file_table.resolve(file);
    return project.build.resolve(file, entry, CommandSource::CDBExact, path, path).command;
}

/// The commands the file's own `entries` compile it as, one per entry.
llvm::SmallVector<CommandID, 2> own_commands(Project& project, Fid file, const auto& entries) {
    llvm::SmallVector<CommandID, 2> commands;
    for(auto& entry: entries) {
        commands.push_back(own_command(project, file, entry.command));
    }
    return commands;
}

/// The commands `host` lends the header `file`, one per command of the
/// host's.
llvm::SmallVector<CommandID, 2> lent_commands(Project& project, Fid file, Fid host) {
    llvm::SmallVector<CommandID, 2> lent;
    for(auto& candidate: host_commands(project, file, host)) {
        lent.push_back(lent_command(project, file, host, candidate.command).command);
    }
    return lent;
}

/// The listing's item for `host` lending the command `applied`, one of the
/// commands `lent` it lends the header: named by the host, the flags that
/// set the command apart when it lends several, and the place it enters
/// the header at when it does at more than one.
ListedContext host_item(Project& project,
                        Fid host,
                        CommandID applied,
                        llvm::ArrayRef<CommandID> lent,
                        std::optional<std::uint32_t> occurrence) {
    auto shown = project.file_table.display(host);
    ListedContext item{.label = std::string(path::filename(shown)), .file = host};
    if(lent.size() > 1) {
        if(auto desc = flags_label(project, applied, lent); !desc.empty()) {
            item.label = std::format("{} [{}]", item.label, desc);
        }
        item.command_hash = project.commands.command_hash_hex(applied);
    }
    if(occurrence) {
        item.label = std::format("{} (#{})", item.label, *occurrence + 1);
        item.occurrence = occurrence;
    }
    item.description = std::move(shown);
    return item;
}

/// The listing's item for the file's own entry `index` of `own`, the
/// commands its entries compile it as.
ListedContext
    own_item(Project& project, Fid file, std::size_t index, llvm::ArrayRef<CommandID> own) {
    auto applied = own[index];
    auto desc = flags_label(project, applied, own);
    return {
        .label = desc.empty() ? std::format("command #{}", index) : desc,
        .description = project.file_table.display(
            CanonicalPath(Spelling::absolute(project.commands.get(applied).directory))),
        .file = file,
        .command_hash = project.commands.command_hash_hex(applied),
    };
}

/// Whether a chosen command still has a live basis among `entry_file`'s
/// candidates: its applied hash matches a candidate under the current edits
/// of `paths` (the host and the header for a host's command), or its
/// recorded base command hash still names one (a rule edit moves every
/// applied hash; the base survives it).
bool command_alive(Project& project,
                   Fid entry_file,
                   llvm::ArrayRef<CanonicalRef> paths,
                   const ContextChoice& choice) {
    auto entry_path = project.file_table.resolve(entry_file);
    for(auto& entry: project.build.commands(entry_file)) {
        if(!choice.base_hash.empty() &&
           project.commands.command_hash_hex(entry.command) == choice.base_hash) {
            return true;
        }
        auto ref =
            project.build.resolve(entry_file, entry.command, entry.source, paths, entry_path);
        if(project.commands.command_hash_hex(ref.command) == choice.command_hash) {
            return true;
        }
    }
    return false;
}

/// The context choices as the database's blob lays them out, field for
/// field: changing these changes the format. Paths persist as portable
/// spellings and are interned again at load.
struct SavedChoice {
    /// Index into the paths table.
    std::uint32_t file;

    /// Index into the paths table; ~0u for none.
    std::uint32_t host;

    std::uint32_t occurrence;

    std::string command_hash;

    std::string base_hash;
};

struct SavedChoices {
    std::vector<std::string> paths;

    std::vector<SavedChoice> contexts;
};

}  // namespace

std::vector<ListedContext> list_contexts(Project& project, Fid file) {
    std::vector<ListedContext> items;

    // Contexts that would produce identical compilation results are
    // collapsed: identical canonical flags mean an identical compile
    // — but only for a header its trial found self-contained under the
    // command. One that needs includer context gets a different
    // synthesized prefix per host, and an un-trialed one may turn out the
    // same way, so every host stays a distinct context for both.
    llvm::StringSet<> seen_commands;
    for(auto host: ranked_hosts(project, file)) {
        // A multi-configuration host contributes one context per CDB
        // entry, and a host entering the header more than once one per
        // place it does: each compiles the header under different
        // preprocessor state. One whose tree no run took yet is listed
        // once.
        auto occurrences = count_occurrences(project, host, file).value_or(1);
        if(occurrences == 0) {
            continue;
        }
        auto lent = lent_commands(project, file, host);
        for(auto applied: lent) {
            auto hash = project.commands.command_hash(applied);
            if(self_contained(project, {file, hash}, true).value_or(false) &&
               !seen_commands.insert(project.commands.command_hash_hex(applied)).second) {
                continue;
            }
            if(occurrences == 1) {
                items.push_back(host_item(project, host, applied, lent, std::nullopt));
                continue;
            }
            for(std::uint32_t n = 0; n < occurrences; n += 1) {
                items.push_back(host_item(project, host, applied, lent, n));
            }
        }
    }

    // Real entries only: lookup() would synthesize a default command
    // even for unknown files, offering a bogus context that
    // switchContext would then reject. Offered even when hosts
    // exist, so a host override can be switched back to the file's
    // own command.
    auto own = own_commands(project, file, project.build.entries(file));
    for(std::size_t i = 0; i < own.size(); i += 1) {
        if(seen_commands.insert(project.commands.command_hash_hex(own[i])).second) {
            items.push_back(own_item(project, file, i, own));
        }
    }
    return items;
}

std::optional<ListedContext> current_context_item(Project& project,
                                                  Fid file,
                                                  const ContextChoice* choice,
                                                  const Resolution& resolution) {
    auto path = project.file_table.resolve(file);
    auto lent = [&](Fid host,
                    llvm::StringRef hash,
                    llvm::StringRef base,
                    std::uint32_t occurrence) -> std::optional<ListedContext> {
        auto applied = host_command(project, file, host, hash, base);
        if(!applied) {
            return std::nullopt;
        }
        auto several = count_occurrences(project, host, file).value_or(1) > 1;
        return host_item(project,
                         host,
                         applied->command,
                         lent_commands(project, file, host),
                         several ? std::optional(occurrence) : std::nullopt);
    };
    auto own = [&](llvm::StringRef hash, llvm::StringRef base) -> std::optional<ListedContext> {
        auto commands = project.build.commands(file);
        if(commands.empty()) {
            return std::nullopt;
        }
        auto entry = pick_chosen_command(project, file, commands, path, path, hash, base);
        auto index = ranges::find_if(commands,
                                     [&](const Candidate& candidate) {
                                         return candidate.command == entry.command;
                                     }) -
                     commands.begin();
        return own_item(project, file, index, own_commands(project, file, commands));
    };

    // The choice, else what resolve_command picks: the file's own command
    // — listed only when it is an entry, not a rule's default — then the
    // host it borrows a command from.
    if(choice && choice->host_path_id.valid()) {
        return lent(choice->host_path_id,
                    choice->command_hash,
                    choice->base_hash,
                    choice->occurrence.value_or(0));
    }
    if(choice) {
        return own(choice->command_hash, choice->base_hash);
    }
    if(project.build.unit(file)) {
        if(project.build.entries(file).empty()) {
            return std::nullopt;
        }
        return own({}, {});
    }
    if(resolution.host.valid()) {
        return lent(resolution.host, {}, {}, resolution.occurrence);
    }
    return std::nullopt;
}

ContextChoice choice_of(Project& project,
                        Fid file,
                        Fid listed,
                        std::optional<std::uint32_t> occurrence,
                        const std::optional<std::string>& command_hash) {
    // The base command hash of the one of `commands` listed as `listed(entry)`
    // under the picked hash: the identity that stays unique when rules
    // collapse two applied hashes onto one value. The caller offers only
    // listed items.
    auto base_of = [&](llvm::ArrayRef<Candidate> commands, auto listed) -> std::string {
        for(auto& entry: commands) {
            if(project.commands.command_hash_hex(listed(entry)) == *command_hash) {
                return project.commands.command_hash_hex(entry.command);
            }
        }
        std::unreachable();
    };

    // Of the listed items only the file's own entries name the file itself,
    // each with its command hash.
    ContextChoice choice;
    if(listed == file) {
        choice.command_hash = *command_hash;
        choice.base_hash = base_of(project.build.commands(file), [&](const Candidate& entry) {
            return own_command(project, file, entry.command);
        });
        return choice;
    }
    choice.host_path_id = listed;
    choice.occurrence = occurrence;
    if(command_hash) {
        choice.command_hash = *command_hash;
        choice.base_hash =
            base_of(host_commands(project, file, listed), [&](const Candidate& entry) {
                return lent_command(project, file, listed, entry.command).command;
            });
    }
    return choice;
}

bool choice_holds(Project& project, Fid file, const ContextChoice& choice) {
    auto path = project.file_table.resolve(file);
    if(choice.host_path_id.valid()) {
        auto host = choice.host_path_id;
        if(project.build.commands(host).empty()) {
            return false;
        }
        // The host's current include tree tells how often its compile
        // enters the file; it is taken only when a compile needs it. Without
        // one, the include graph tells whether the host still includes the
        // file — all but a macro include, which only a tree sees.
        auto occurrences = count_occurrences(project, host, file);
        if(occurrences == 0 ||
           (!occurrences && !project.dep_graph.include_closure(host).contains(file))) {
            return false;
        }
        // A chosen occurrence can vanish while other inclusions of the
        // header survive.
        if(occurrences && choice.occurrence.has_value() && *choice.occurrence >= *occurrences) {
            return false;
        }
        CanonicalRef edit_paths[] = {project.file_table.resolve(host), path};
        return choice.command_hash.empty() || command_alive(project, host, edit_paths, choice);
    }
    return !choice.command_hash.empty() && !project.build.commands(file).empty() &&
           command_alive(project, file, path, choice);
}

std::string serialize_choices(Project& project,
                              llvm::ArrayRef<std::pair<Fid, const ContextChoice*>> choices) {
    SavedChoices data;
    llvm::StringMap<std::uint32_t> indices;
    auto intern = [&](Fid fid) -> std::uint32_t {
        auto path = project.index.portable(project.file_table.resolve(fid));
        auto [it, inserted] =
            indices.try_emplace(path, static_cast<std::uint32_t>(data.paths.size()));
        if(inserted) {
            data.paths.push_back(path);
        }
        return it->second;
    };
    for(auto& [file, choice]: choices) {
        data.contexts.push_back({
            .file = intern(file),
            .host = choice->host_path_id.valid() ? intern(choice->host_path_id) : ~0u,
            .occurrence = choice->occurrence.value_or(~0u),
            .command_hash = choice->command_hash,
            .base_hash = choice->base_hash,
        });
    }
    auto json = json::to_string<PathJsonConfig>(data);
    if(!json) {
        LOG_WARN("Failed to serialize the context choices");
        return {};
    }
    return std::move(*json);
}

std::vector<std::pair<Fid, ContextChoice>> parse_choices(Project& project, llvm::StringRef bytes) {
    std::vector<std::pair<Fid, ContextChoice>> choices;
    if(bytes.empty()) {
        return choices;
    }
    SavedChoices data;
    if(!json::from_string(bytes, data)) {
        LOG_WARN("Failed to parse the context choices");
        return choices;
    }
    auto path = [&](std::uint32_t index) -> llvm::StringRef {
        return index < data.paths.size() ? llvm::StringRef(data.paths[index]) : "";
    };
    for(auto& saved: data.contexts) {
        auto file = path(saved.file);
        if(file.empty()) {
            continue;
        }
        ContextChoice choice;
        if(saved.host != ~0u) {
            auto host = path(saved.host);
            if(host.empty()) {
                continue;
            }
            choice.host_path_id = project.file_table.intern(project.index.local(host));
        }
        if(saved.occurrence != ~0u) {
            choice.occurrence = saved.occurrence;
        }
        choice.command_hash = saved.command_hash;
        choice.base_hash = saved.base_hash;
        choices.emplace_back(project.file_table.intern(project.index.local(file)),
                             std::move(choice));
    }
    return choices;
}

}  // namespace clice

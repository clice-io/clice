module clice;

import :index.project_index;
import :project.command;
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

/// The commands `candidates` compile `file` as, `host`'s passed on to it for
/// a host.
llvm::SmallVector<CommandID, 2>
    applied(Project& project, Fid file, Fid host, llvm::ArrayRef<Candidate> candidates) {
    llvm::SmallVector<CommandID, 2> commands;
    for(auto& candidate: candidates) {
        commands.push_back(project
                               .apply_rules(file,
                                            host.valid()
                                                ? candidate.passed_on(host, CommandSource::Host)
                                                : candidate)
                               .command);
    }
    return commands;
}

/// The listing's item for `host` passing on its `index`th of `offered`, the
/// commands it can compile the header `file` under: named by the host, the
/// flags that set the command apart when there are several, and the place
/// it enters the header at when it does at more than one.
ListedContext host_item(Project& project,
                        Fid file,
                        Fid host,
                        llvm::ArrayRef<Candidate> offered,
                        std::size_t index,
                        std::optional<std::uint32_t> occurrence) {
    auto shown = project.file_table.display(host);
    ListedContext item{.label = std::string(path::filename(shown)), .file = host};
    if(offered.size() > 1) {
        auto commands = applied(project, file, host, offered);
        if(auto desc = flags_label(project, commands[index], commands); !desc.empty()) {
            item.label = std::format("{} [{}]", item.label, desc);
        }
        item.command_hash = project.commands.command_hash_hex(offered[index].base);
    }
    if(occurrence) {
        item.label = std::format("{} (#{})", item.label, *occurrence + 1);
        item.occurrence = occurrence;
    }
    item.description = std::move(shown);
    return item;
}

/// The listing's item for the file's own entry `index` of `own`.
ListedContext
    own_item(Project& project, Fid file, std::size_t index, llvm::ArrayRef<Candidate> own) {
    auto commands = applied(project, file, {}, own);
    auto desc = flags_label(project, commands[index], commands);
    return {
        .label = desc.empty() ? std::format("command #{}", index) : desc,
        .description = project.file_table.display(
            CanonicalPath(Spelling::absolute(project.commands.get(commands[index]).directory))),
        .file = file,
        .command_hash = project.commands.command_hash_hex(own[index].base),
    };
}

/// The file's own database entries, as candidates.
llvm::SmallVector<Candidate, 2> own_entries(Project& project, Fid file) {
    llvm::SmallVector<Candidate, 2> own;
    for(auto& entry: project.entries(file)) {
        own.push_back({.base = entry.command, .source = CommandSource::Database});
    }
    return own;
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

std::vector<ListedContext> Project::list_contexts(Fid file) {
    std::vector<ListedContext> items;

    // Contexts that would produce identical compilation results are
    // collapsed: identical canonical flags mean an identical compile
    // — but only for a header its check found self-contained under the
    // command. One that needs includer context gets a different
    // synthesized prefix per host, and an unchecked one may turn out the
    // same way, so every host stays a distinct context for both.
    llvm::StringSet<> seen_commands;
    for(auto host: ranked_hosts(file)) {
        // A multi-configuration host contributes one context per CDB
        // entry, and a host entering the header more than once one per
        // place it does: each compiles the header under different
        // preprocessor state. One whose tree no run took yet is listed
        // once.
        auto occurrences = count_occurrences(host, file).value_or(1);
        if(occurrences == 0) {
            continue;
        }
        auto offered = candidates(file, host);
        auto commands = applied(*this, file, host, offered);
        for(std::size_t i = 0; i < offered.size(); i += 1) {
            if(self_contained({file, this->commands.command_hash(commands[i])}, true)
                   .value_or(false) &&
               !seen_commands.insert(this->commands.command_hash_hex(commands[i])).second) {
                continue;
            }
            if(occurrences == 1) {
                items.push_back(host_item(*this, file, host, offered, i, std::nullopt));
                continue;
            }
            for(std::uint32_t n = 0; n < occurrences; n += 1) {
                items.push_back(host_item(*this, file, host, offered, i, n));
            }
        }
    }

    // Real entries only: a default command is no context to switch to.
    // Offered even when hosts exist, so a host override can be switched back
    // to the file's own command.
    auto own = own_entries(*this, file);
    auto commands = applied(*this, file, {}, own);
    for(std::size_t i = 0; i < own.size(); i += 1) {
        if(seen_commands.insert(this->commands.command_hash_hex(commands[i])).second) {
            items.push_back(own_item(*this, file, i, own));
        }
    }
    return items;
}

std::optional<ListedContext> Project::current_context_item(Fid file, const Resolution& resolution) {
    // What the resolution picked: the host the file compiles in, else the
    // file's own entry — not a rule's default command or a borrowed one.
    auto index_of = [&](llvm::ArrayRef<Candidate> offered) {
        auto it = ranges::find(offered, resolution.candidate.base, &Candidate::base);
        return it != offered.end() ? std::optional(it - offered.begin()) : std::nullopt;
    };
    if(auto host = resolution.host; host.valid()) {
        auto offered = candidates(file, host);
        auto index = index_of(offered);
        if(!index) {
            return std::nullopt;
        }
        auto several = count_occurrences(host, file).value_or(1) > 1;
        return host_item(*this,
                         file,
                         host,
                         offered,
                         *index,
                         several ? std::optional(resolution.occurrence) : std::nullopt);
    }
    if(resolution.candidate.source != CommandSource::Database) {
        return std::nullopt;
    }
    auto own = own_entries(*this, file);
    auto index = index_of(own);
    return index ? std::optional(own_item(*this, file, *index, own)) : std::nullopt;
}

bool Project::choice_holds(Fid file, const ContextChoice& choice) {
    if(auto host = choice.host; host.valid()) {
        auto offered = candidates(file, host);
        if(offered.empty()) {
            return false;
        }
        // The host's current include tree tells how often its compile
        // enters the file; it is taken only when a compile needs it. Without
        // one, the include graph tells whether the host still includes the
        // file — all but a macro include, which only a tree sees.
        auto occurrences = count_occurrences(host, file);
        if(occurrences == 0 || (!occurrences && !dep_graph.include_closure(host).contains(file))) {
            return false;
        }
        // A chosen occurrence can vanish while other inclusions of the
        // header survive.
        if(occurrences && choice.occurrence.has_value() && *choice.occurrence >= *occurrences) {
            return false;
        }
        return choice.base.empty() || chosen_candidate(offered, choice.base).has_value();
    }
    return !choice.base.empty() && chosen_candidate(own_entries(*this, file), choice.base);
}

std::string
    Project::serialize_choices(llvm::ArrayRef<std::pair<Fid, const ContextChoice*>> choices) {
    SavedChoices data;
    llvm::StringMap<std::uint32_t> indices;
    auto intern = [&](Fid fid) -> std::uint32_t {
        auto path = index.portable(file_table.resolve(fid));
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
            .host = choice->host.valid() ? intern(choice->host) : ~0u,
            .occurrence = choice->occurrence.value_or(~0u),
            .base_hash = choice->base,
        });
    }
    auto json = json::to_string<PathJsonConfig>(data);
    if(!json) {
        LOG_WARN("Failed to serialize the context choices");
        return {};
    }
    return std::move(*json);
}

std::vector<std::pair<Fid, ContextChoice>> Project::parse_choices(llvm::StringRef bytes) {
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
        // A choice of a command saved before its base was recorded no longer
        // names one.
        if(file.empty() || (saved.base_hash.empty() && !saved.command_hash.empty())) {
            continue;
        }
        ContextChoice choice;
        if(saved.host != ~0u) {
            auto host = path(saved.host);
            if(host.empty()) {
                continue;
            }
            choice.host = file_table.intern(index.local(host));
        }
        if(saved.occurrence != ~0u) {
            choice.occurrence = saved.occurrence;
        }
        choice.base = saved.base_hash;
        choices.emplace_back(file_table.intern(index.local(file)), std::move(choice));
    }
    return choices;
}

}  // namespace clice

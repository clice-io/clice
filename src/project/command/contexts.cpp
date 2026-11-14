module clice;

import :command.argument;
import :index.project_index;
import :project.command;
import :project.project;
import :project.saved;
import :support.json;
import :support.logging;
import :syntax.dependency_graph;
import :vfs.path;

namespace clice {

namespace {

/// The flags a context's label tells a command by: its macro definitions,
/// optimization levels and language standards, clang's or cl's, each in
/// one piece (`-DNAME=1`, `-O2`, `/std:c++20`).
llvm::SmallVector<std::string> state_flags(Project& project, CommandID command) {
    llvm::SmallVector<std::string> flags;
    for(auto& arg: project.commands.get(command).args) {
        using namespace clang_options;
        auto id = arg.opt_id;
        if(id != OPT_D && id != OPT_std_EQ && id != OPT__SLASH_std && id != OPT__SLASH_O &&
           !table().option(id)->matches(OPT_O_Group)) {
            continue;
        }
        auto& flag = flags.emplace_back();
        render_arg(arg, [&](std::string_view part) { flag += part; });
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

    // LIMITATION: a source that includes itself (one table expanded twice)
    // is offered its own command alone, not the place it enters itself: how
    // the second pass reads the code both passes compile is never shown from
    // the AST.
    //
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

void Project::save_choices(llvm::ArrayRef<std::pair<Fid, const ContextChoice*>> choices) {
    SavedChoices data;
    PathTable paths(index, file_table, data.paths);
    for(auto& [file, choice]: choices) {
        data.contexts.push_back({
            .file = paths.add(file),
            .host = choice->host.valid() ? paths.add(choice->host) : ~0u,
            .occurrence = choice->occurrence.value_or(~0u),
            .base_hash = choice->base,
        });
    }
    auto json = json::to_string(data, path_json);
    if(!json) {
        LOG_WARN("Failed to serialize the context choices");
    }
    this->choices.bytes = json ? std::move(*json) : std::string();
    this->choices.dirty = true;
    on_metadata_changed.emit();
}

std::vector<std::pair<Fid, ContextChoice>> Project::saved_choices() {
    std::vector<std::pair<Fid, ContextChoice>> choices;
    auto& bytes = this->choices.bytes;
    if(bytes.empty()) {
        return choices;
    }
    SavedChoices data;
    if(!json::from_string(bytes, data)) {
        LOG_WARN("Failed to parse the context choices");
        return choices;
    }
    PathTable paths(index, file_table, data.paths);
    for(auto& saved: data.contexts) {
        auto file = paths.file(saved.file);
        // A choice of a command saved before its base was recorded no longer
        // names one.
        if(!file || (saved.base_hash.empty() && !saved.command_hash.empty())) {
            continue;
        }
        ContextChoice choice;
        if(saved.host != ~0u) {
            auto host = paths.file(saved.host);
            if(!host) {
                continue;
            }
            choice.host = *host;
        }
        if(saved.occurrence != ~0u) {
            choice.occurrence = saved.occurrence;
        }
        choice.base = saved.base_hash;
        choices.emplace_back(*file, std::move(choice));
    }
    return choices;
}

}  // namespace clice

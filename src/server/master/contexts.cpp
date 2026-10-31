module clice;

import :feature.protocol;
import :project.contexts;
import :project.project;
import :server.master;
import :server.served_project;
import :support.logging;
import :vfs.path;

namespace clice {

/// The protocol's form of a listed context.
static ext::ContextItem to_item(const FileTable& files, ListedContext listed) {
    return {
        .label = std::move(listed.label),
        .description = std::move(listed.description),
        .uri = feature::to_uri(files.display(listed.file)),
        .occurrence = listed.occurrence,
        .command_hash = std::move(listed.command_hash),
    };
}

const MasterServer::ChoiceEntry* MasterServer::choice(Fid path_id) const {
    auto it = choices.find(path_id);
    return it != choices.end() ? &it->second : nullptr;
}

void MasterServer::load_choices(ServedProject& project) {
    bool dropped = false;
    for(auto& [file, saved]: parse_choices(project.project, project.project.choices.bytes)) {
        auto [it, inserted] = choices.try_emplace(file);
        auto& entry = it->second;
        // Of two projects that saved a choice for one file, the deeper root
        // wins.
        if(!inserted && entry.project->project.root.size() >= project.project.root.size()) {
            dropped = true;
            continue;
        }
        if(!inserted) {
            LOG_INFO("Both {} and {} saved a context choice for {}; the deeper one stands",
                     entry.project->project.root,
                     project.project.root,
                     files.resolve(file));
            auto* other = entry.project;
            entry = {};
            save_choices(*other);
        }
        bool active = choice_holds(project.project, file, saved);
        entry = {.choice = std::move(saved), .project = &project, .active = active};
    }
    if(dropped) {
        save_choices(project);
    }
}

bool MasterServer::recheck_choice(Fid path_id) {
    auto it = choices.find(path_id);
    if(it == choices.end()) {
        return false;
    }
    auto& entry = it->second;
    bool active = choice_holds(entry.project->project, path_id, entry.choice);
    if(active == entry.active) {
        return false;
    }
    LOG_INFO("The context choice for {} {}",
             files.resolve(path_id),
             active ? "holds again" : "no longer holds; it waits, inactive");
    entry.active = active;
    return true;
}

void MasterServer::save_choices(ServedProject& project) {
    llvm::SmallVector<std::pair<Fid, const ContextChoice*>> saved;
    for(auto& [file, entry]: choices) {
        if(entry.project == &project) {
            saved.emplace_back(file, &entry.choice);
        }
    }
    auto& blob = project.project.choices;
    blob.bytes = serialize_choices(project.project, saved);
    blob.dirty = true;
    if(project.project.request_flush) {
        project.project.request_flush();
    }
}

void MasterServer::forget_choices(ServedProject& project) {
    llvm::SmallVector<Fid> files;
    for(auto& [file, entry]: choices) {
        if(entry.project == &project) {
            files.push_back(file);
        }
    }
    for(auto file: files) {
        choices.erase(file);
    }
}

std::uint64_t MasterServer::context_epoch() {
    // Each project's epoch only grows, so their sum moves with any of them
    // while the projects stay the same.
    std::uint64_t sum = 0;
    for(auto& project: projects) {
        sum += project->project.context_epoch;
    }
    if(std::pair(projects_generation, sum) != context_seen) {
        context_seen = {projects_generation, sum};
        context_generation += 1;
    }
    return context_generation;
}

ext::QueryContextResult MasterServer::query_contexts(Fid path_id,
                                                     const ext::QueryContextParams& params) {
    constexpr std::size_t page_size = 10;
    auto& owner = owner_of(path_id);
    auto items = list_contexts(owner.project, path_id);
    for(auto& project: projects) {
        if(project.get() == &owner) {
            continue;
        }
        for(auto& item: list_contexts(project->project, path_id)) {
            if(ranges::none_of(items, [&](const ListedContext& known) {
                   return known.file == item.file && known.occurrence == item.occurrence &&
                          known.command_hash == item.command_hash;
               })) {
                items.push_back(std::move(item));
            }
        }
    }
    ext::QueryContextResult result;
    result.epoch = context_epoch();
    result.total = static_cast<int>(items.size());
    auto offset = static_cast<std::size_t>(std::max(0, params.offset.value_or(0)));
    for(auto i = offset; i < std::min(offset + page_size, items.size()); i += 1) {
        result.contexts.push_back(to_item(files, std::move(items[i])));
    }
    return result;
}

kota::task<ext::SwitchContextResult> MasterServer::switch_context(Fid path_id,
                                                                  Fid context_path_id,
                                                                  ext::SwitchContextParams params) {
    ext::SwitchContextResult result;
    // A choice made against an outdated listing may reference contexts
    // that no longer exist — make the client re-query.
    if(params.epoch && *params.epoch != context_epoch()) {
        result.stale = true;
        co_return result;
    }

    // The project offering the chosen item: the file's own first, then the
    // others, in the order query_contexts listed them.
    auto owner = owner_of(path_id).shared_from_this();
    auto offers = [&](ServedProject& project) -> kota::task<bool> {
        // The listing counts the places the host's compile enters the file
        // by its include tree.
        ContextChoice wanted{.host_path_id = context_path_id};
        std::string directory;
        std::vector<std::string> arguments;
        co_await project.document_command(path_id, directory, arguments, &wanted);
        co_return ranges::any_of(list_contexts(project.project, path_id),
                                 [&](const ListedContext& item) {
                                     return item.file == context_path_id &&
                                            item.occurrence == params.occurrence &&
                                            item.command_hash == params.command_hash;
                                 });
    };
    auto target = owner;
    if(!co_await offers(*owner)) {
        target.reset();
        // Copied: a project may come or go while a tree is taken.
        auto candidates = projects;
        for(auto& project: candidates) {
            if(co_await offers(*project)) {
                target = project;
                break;
            }
        }
        if(!target) {
            co_return result;
        }
    }
    // A choice applies to an open document; one closed meanwhile keeps the
    // choice it had.
    auto* document = find_document(path_id);
    if(!document || !document->owner) {
        co_return result;
    }
    auto* previous = choice(path_id) ? choice(path_id)->project : nullptr;
    choices[path_id] = {
        .choice = choice_of(target->project,
                            path_id,
                            context_path_id,
                            params.occurrence,
                            params.command_hash),
        .project = target.get(),
    };
    save_choices(*target);
    if(previous && previous != target.get()) {
        save_choices(*previous);
    }
    if(document->owner != target.get()) {
        change_owner(*document, *target);
    } else {
        target->choice_changed(*document->session);
    }
    // A context choice asks for the context-pure AST view; the merged
    // index cannot give it (union entries).
    target->escalate(path_id);
    replace_shown(*document);
    result.success = true;
    co_return result;
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

kota::task<ext::CurrentContextResult> MasterServer::current_context(Fid path_id) {
    ext::CurrentContextResult result;
    auto* document = find_document(path_id);
    if(!document || !document->owner) {
        result.epoch = context_epoch();
        co_return result;
    }
    auto owner = document->owner->shared_from_this();
    auto& project = owner->project;
    auto& command = result.command.emplace();
    std::vector<std::string> resolved;
    // As the document's compile does, with the tree of the host it waits
    // for: the compile in flight takes the same one.
    auto resolution = co_await owner->document_command(path_id, command.directory, resolved);
    // The command line as its database or rule writes it, the rules
    // applied, rather than the frontend's the compile gets: the one a user
    // recognizes, and could run.
    auto file = project.file_table.spelling(path_id).str();
    for(auto* argument: project.commands.render_full(resolution.ref.command, file)) {
        command.arguments.emplace_back(argument);
    }
    command.source = source_name(resolution.source);
    auto from = resolution.host.valid() ? resolution.host : resolution.lender;
    if(from.valid()) {
        command.from = feature::to_uri(project.file_table.display(from));
    }
    if(!resolution.query_failure.empty()) {
        command.query_failure = std::move(resolution.query_failure);
    }
    auto* active = owner->active_choice(path_id);
    result.automatic = active == nullptr;
    if(auto item = current_context_item(project, path_id, active, resolution)) {
        result.context = to_item(files, std::move(*item));
    }
    if(auto* entry = choice(path_id); entry && !entry->active) {
        auto& waiting = entry->choice;
        auto chosen = waiting.host_path_id.valid() ? waiting.host_path_id : path_id;
        auto shown = files.display(chosen);
        result.inactive_choice = ext::ContextItem{
            .label = std::string(path::filename(shown)),
            .description = shown,
            .uri = feature::to_uri(shown),
            .occurrence = waiting.occurrence,
            .command_hash =
                waiting.command_hash.empty() ? std::nullopt : std::optional(waiting.command_hash),
        };
    }
    result.epoch = context_epoch();
    co_return result;
}

ext::SwitchContextResult MasterServer::reset_context(Fid path_id) {
    auto* document = find_document(path_id);
    if(!document || !document->owner) {
        return {};
    }
    auto it = choices.find(path_id);
    if(it == choices.end()) {
        return {.success = true};
    }
    auto* project = it->second.project;
    choices.erase(it);
    save_choices(*project);
    auto& owner = *document->owner;
    if(auto& to = route(path_id); &to != &owner) {
        change_owner(*document, to);
    } else {
        owner.choice_changed(*document->session);
        replace_shown(*document);
    }
    return {.success = true};
}

}  // namespace clice

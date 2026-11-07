module clice;

import :project.background;
import :project.command;
import :server.client;
import :server.master;
import :server.served_project;
import :vfs.file_table;

namespace clice {

llvm::SmallVector<OpenFile> ServedProject::open_files() const {
    llvm::SmallVector<OpenFile> open;
    for(auto& [path_id, document]: documents) {
        auto& file = open.emplace_back(OpenFile{.file = path_id});
        each_input(path_id, [&](const Inputs& inputs) {
            for(auto& input: inputs) {
                file.read.push_back(input.file);
            }
        });
    }
    return open;
}

void ServedProject::disk_changed(llvm::ArrayRef<Fid> files) {
    if(broken()) {
        return;
    }
    auto open = open_files();
    auto known = ranges::to<llvm::SmallVector<Fid>>(
        views::filter(files, [&](Fid file) { return project.knows(file, open); }));
    if(!known.empty()) {
        follow(project.files_changed(known, open));
    }
}

void ServedProject::commands_changed(CDBDiff delta) {
    follow(project.update_members(std::move(delta), open_files()));
}

void ServedProject::follow(const Affected& affected) {
    bool refresh = false;
    auto mark_stale = [&](ServedDocument& document) {
        inputs_changed(*document.session);
        refresh = refresh || document.serving == ServingMode::Escalated;
    };
    for(auto path_id: affected.stale) {
        mark_stale(*find_document(path_id));
    }
    for(auto path_id: affected.reindexed) {
        book_background_diagnostics(*find_document(path_id), false);
    }
    // An open document whose command moved recompiles: one whose entry
    // appeared, changed or left, a header whose host's command did or that
    // another host now ranks before it, a file borrowing a member's. One
    // whose first compile is still in flight may have resolved before.
    auto& commands = affected.commands;
    if(!commands.empty()) {
        for(auto& [path_id, document]: documents) {
            auto selected = project.select_command(path_id, command_request(path_id));
            auto& latest = document.latest;
            if(!latest || latest->command != selected.ref.command ||
               latest->host != selected.host) {
                mark_stale(document);
            }
        }
    }

    // A change can remove the include edge or the entry a user's context
    // choice depends on, or bring it back.
    recheck_choices();
    project.background_index.schedule();
    if(refresh && server.client) {
        server.client->refresh_diagnostics();
        server.client->refresh();
    }
    if(commands.empty()) {
        return;
    }

    // The database gained or lost entries: an open document may belong to
    // another project now.
    if(!commands.added.empty() || !commands.removed.empty()) {
        server.reroute();
    }
    // A database found or a member recorded: the directories on the way to
    // it decide what loads too.
    server.watch_settings();
}

void ServedProject::recheck_choices() {
    bool rerouted = false;
    for(auto& [file, entry]: server.choices) {
        if(entry.project != this || !server.recheck_choice(file)) {
            continue;
        }
        rerouted = true;
        // Whether the choice holds is part of what the document compiles
        // under.
        if(auto* document = find_document(file)) {
            choice_changed(*document->session);
        }
    }
    if(rerouted) {
        server.reroute();
    }
}

std::size_t ServedProject::reload_databases(bool force) {
    if(!watching) {
        return 0;
    }
    // A reload would record the other build's commands as the ones this
    // index was built with.
    if(project.databases_moved()) {
        server.ask_restart({
            .file = settings_file,
            .message = std::format("The compilation databases of {} now resolve to another build",
                                   project.file_table.display(project.root)),
        });
        return 0;
    }
    auto delta = project.look_at_databases(
        force,
        ranges::to<llvm::SmallVector<Fid>>(
            views::transform(documents, [](auto& entry) { return entry.first; })));
    if(delta.empty()) {
        return 0;
    }
    commands_changed(std::move(delta));
    return 1;
}

async::task<bool> ServedProject::poll_sources() {
    if(!watching) {
        co_return false;
    }
    auto walked =
        co_await async::queue([walk = project.rules.source_walk()] { return walk_sources(walk); });
    auto added = project.refresh_default_sources(walked);
    if(added.empty()) {
        co_return false;
    }
    commands_changed({.added = std::move(added)});
    co_return true;
}

void ServedProject::discover_around(Fid path_id) {
    if(!watching) {
        return;
    }
    if(auto found = project.discover_databases({path_id}, {.load_now = true}); !found.empty()) {
        commands_changed(std::move(found));
    }
}

std::vector<CanonicalPath> ServedProject::configuration_dirs() const {
    return project.configuration_dirs(ranges::to<llvm::SmallVector<Fid>>(
        views::transform(documents, [](auto& entry) { return entry.first; })));
}

}  // namespace clice

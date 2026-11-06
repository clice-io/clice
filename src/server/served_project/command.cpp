module clice;

import :project.command;
import :project.project;
import :server.master;
import :server.served_project;

namespace clice {

CommandRequest ServedProject::command_request(Fid path_id, const ContextChoice* candidate) const {
    return {.choice = candidate ? candidate : active_choice(path_id), .open = true};
}

const ContextChoice* ServedProject::active_choice(Fid path_id) const {
    auto* entry = server.choice(path_id);
    return entry && entry->project == this && entry->active ? &entry->choice : nullptr;
}

Fid ServedProject::host_of(Fid path_id) {
    if(auto* choice = active_choice(path_id); choice && choice->host.valid()) {
        return choice->host;
    }
    return project.host_of(path_id);
}

const clang::LangOptions& ServedProject::dialect(Fid path_id) {
    return project.dialect(path_id, active_choice(path_id));
}

}  // namespace clice

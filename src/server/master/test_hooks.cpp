module clice;

import :project.task_graph;
import :server.master;
import :server.served_project;
import :worker.probe;

namespace clice {

/// Whether the project's background index has work wait_idle() waits for.
static bool indexing(ServedProject& project) {
    return !project.project.background_index.idle();
}

/// The requests of `editor` wait_idle() waits for: all but syncs.
static auto awaited_requests(const ipc::JSONPeer& editor) {
    auto requests = editor.incoming_requests();
    std::erase_if(requests,
                  [](const auto& request) { return request.method == "clice/internal/sync"; });
    return requests;
}

async::task<> MasterServer::wait_idle(const ipc::JSONPeer& editor) {
    auto quiet = [&] {
        apply_disk_changes();
        return awaited_requests(editor).empty() && waiting_projects.empty() &&
               ranges::none_of(projects,
                               [](auto& project) {
                                   return !project->project.graph.compiling().empty() ||
                                          indexing(*project);
                               }) &&
               ranges::all_of(removed_projects, [](auto& weak) { return weak.expired(); });
    };
    while(true) {
        if(quiet()) {
            // What a commit scheduled on the loop — a refresh request, a
            // progress handshake — goes out before the answer.
            co_await async::yield();
            if(quiet()) {
                co_return;
            }
        }
        // No event: none of them announces its end.
        co_await async::sleep(chrono::milliseconds(1), loop);
    }
}

std::vector<std::string> MasterServer::pending_work(const ipc::JSONPeer& editor) {
    std::vector<std::string> lines;
    for(auto& request: awaited_requests(editor)) {
        lines.push_back(std::visit(
            [&](const auto& id) { return std::format("request {} {}", request.method, id); },
            request.id));
    }
    for(auto& weak: removed_projects) {
        if(auto project = weak.lock()) {
            lines.push_back(std::format("removed project {} shutting down", project->project.root));
        }
    }
    for(auto* project: waiting_projects) {
        lines.push_back(
            std::format("project {} waiting for its previous one to go", project->project.root));
    }
    for(auto& hold: probe.holds()) {
        if(hold.parked) {
            lines.push_back(std::format("{} {}: reply parked by hold {}",
                                        build_kind_name(hold.kind),
                                        files.display_identity(hold.file),
                                        hold.id));
        }
    }
    for(auto& gate: probe.gates()) {
        if(gate.parked && !gate.worker.expired()) {
            lines.push_back(
                std::format("{}: work parked in its worker by gate {}", gate.tag, gate.id));
        }
    }
    for(auto& project: projects) {
        for(auto id: project->project.graph.compiling()) {
            auto file = [&] {
                return files.display(id.file());
            };
            switch(id.kind) {
                case NodeKind::AST: lines.push_back(std::format("compile {}", file())); break;
                case NodeKind::Unit: lines.push_back(std::format("index {}", file())); break;
                case NodeKind::PCM: lines.push_back(std::format("pcm {}", file())); break;
                case NodeKind::PCH: lines.push_back(std::format("pch #{}", id.key)); break;
            }
        }
        if(indexing(*project)) {
            auto& background = project->project.background_index;
            lines.push_back(std::format("index pass at {}/{}, {} files queued",
                                        background.progress().completed,
                                        background.progress().total,
                                        background.queue().queued()));
        }
    }
    return lines;
}

}  // namespace clice

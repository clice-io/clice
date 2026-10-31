module clice;

import :sched.graph;
import :server.master;
import :server.served_project;
import :worker.probe;

namespace clice {

/// Whether the project's index pump has work wait_idle() waits for. A queue
/// no round will take, indexing being off, is none.
static bool indexing(ServedProject& project) {
    return project.project.settings.project.enable_indexing.value && !project.sched.pump.is_idle();
}

/// The requests of `editor` wait_idle() waits for: all but syncs.
static auto awaited_requests(const ipc::JSONPeer& editor) {
    auto requests = editor.incoming_requests();
    std::erase_if(requests,
                  [](const auto& request) { return request.method == "clice/internal/sync"; });
    return requests;
}

kota::task<> MasterServer::wait_idle(const ipc::JSONPeer& editor) {
    auto quiet = [&] {
        apply_disk_changes();
        return awaited_requests(editor).empty() && waiting_projects.empty() &&
               ranges::none_of(projects,
                               [](auto& project) {
                                   return !project->sched.graph.compiling().empty() ||
                                          indexing(*project);
                               }) &&
               ranges::all_of(removed_projects, [](auto& weak) { return weak.expired(); });
    };
    while(true) {
        if(quiet()) {
            // What a commit scheduled on the loop — a refresh request, a
            // progress handshake — goes out before the answer.
            co_await kota::yield();
            if(quiet()) {
                co_return;
            }
        }
        // No event: none of them announces its end.
        co_await kota::sleep(std::chrono::milliseconds(1), loop);
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
        for(auto id: project->sched.graph.compiling()) {
            auto file = [&] {
                return files.display(Fid{static_cast<std::uint32_t>(id.key)});
            };
            switch(id.family) {
                case Family::AST: lines.push_back(std::format("compile {}", file())); break;
                case Family::TURun: lines.push_back(std::format("index {}", file())); break;
                case Family::PCM: lines.push_back(std::format("pcm {}", file())); break;
                case Family::PCH: lines.push_back(std::format("pch #{}", id.key)); break;
            }
        }
        if(indexing(*project)) {
            auto& pump = project->sched.pump;
            lines.push_back(std::format("index round at {}/{}, {} files queued",
                                        pump.progress().completed,
                                        pump.progress().total,
                                        pump.pending_files()));
        }
    }
    return lines;
}

}  // namespace clice

module clice;

import :server.master;
import :server.served_project;
import :worker.probe;

namespace clice {

kota::task<> MasterServer::poll_task() {
    // A tick's looks run on the event loop: bounded, so a backlog (a
    // checkout making every file of a project due) spreads over ticks instead
    // of stalling requests.
    constexpr auto interval = std::chrono::milliseconds(250);
    constexpr auto budget = std::chrono::milliseconds(2);
    while(true) {
        // Time is the feature: the disk loop's pace.
        co_await kota::sleep(interval);
        if(polling_files) {
            if(files.disk.tick(budget) > 0) {
                probe.happened(LoopEvent::DiskChange);
            }
            for(auto& project: projects) {
                project->tick_databases();
            }
        }
        look_at_settings();
    }
}

std::size_t MasterServer::apply_disk_changes() {
    auto changed = files.disk.take_changes();
    for(auto& project: projects) {
        llvm::SmallVector<Fid> known;
        for(auto path_id: changed) {
            if(project->knows(path_id)) {
                known.push_back(path_id);
            }
        }
        if(!known.empty()) {
            project->disk_changed(known);
        }
    }
    return changed.size();
}

}  // namespace clice

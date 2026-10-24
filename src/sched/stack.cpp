module;

#include "support/prelude.h"

module clice;

import :index.project_index;
import :sched.stack;
import :support.environment;

namespace clice {

SchedulingStack::SchedulingStack(kota::event_loop& loop,
                                 Project& project,
                                 CommandResolver& commands,
                                 WorkerPool& pool) :
    project(project), pool(pool), pcm(graph, project, commands, pool), pch(graph, project, pool),
    turun(graph, project, commands, pcm, pch, pool), pump(loop, project, commands, turun, pool) {
    pcm.register_runner();
    pch.register_runner();
    turun.register_runner();
    pcm.on_indexing_needed = [this] {
        pump.schedule();
    };
}

std::chrono::milliseconds SchedulingStack::checkpoint_interval() {
    // Lets integration tests kill a server between checkpoints.
    if(auto ms = env_integer("CLICE_TEST_CHECKPOINT_MS")) {
        return std::chrono::milliseconds(*ms);
    }
    return std::chrono::minutes(5);
}

kota::task<> SchedulingStack::checkpoint() {
    if(project.store) {
        co_await kota::queue([this] { project.store->checkpoint(); });
    }
    // A full search build or a fold here could still be running on the
    // thread pool when a shutdown cancels the checkpoint, holding up the
    // final save behind seconds of derived work: the round end does them.
    co_await pump.persist(index::SearchRebuild::Never);
    // Repair debt the save's compaction discovered needs a round of its own
    // when none is running.
    pump.schedule();
}

kota::task<> SchedulingStack::shutdown() {
    co_await graph.shutdown();
    // Editors kill a server that is slow to exit, and building the search
    // index dominates a cold round's save: the index commits with no more
    // than a run of the changes since the last save, and the next session's
    // first idle save builds the rest.
    co_await pump.persist(index::SearchRebuild::Never);
}

void SchedulingStack::close() {
    if(project.store) {
        project.store->shutdown();
    }
}

}  // namespace clice

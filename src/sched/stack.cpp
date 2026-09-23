#include "sched/stack.h"

namespace clice {

SchedulingStack::SchedulingStack(kota::event_loop& loop,
                                 Project& project,
                                 CommandResolver& commands) :
    project(project), pool(loop), graph(loop), pcm(graph, project, commands, pool),
    pch(graph, project, pool), store(loop, project, commands),
    turun(graph, project, commands, pcm, store, pool), pump(loop, project, turun, store, pool) {
    pcm.register_runner();
    pch.register_runner();
    turun.register_runner();
    pcm.on_indexing_needed = [this] {
        pump.schedule();
    };
}

kota::task<> SchedulingStack::shutdown() {
    co_await graph.shutdown();
    auto report = co_await store.save(pump.save_debt(), /*settle=*/true);
    pump.claim_report(report);
    if(report.snapshot_stale) {
        // Debt surfaced after the snapshot serialized (write-time
        // corruption recovery): one metadata retry, or a dropped
        // standalone header's repair debt dies with this process.
        pump.claim_report(co_await store.save(pump.save_debt(), /*settle=*/true));
    }
    co_await pool.stop();
    if(project.store) {
        project.store->shutdown();
    }
}

}  // namespace clice

module;

#include "modules/prelude.h"

module clice:sched.stack;

import :project.command_resolver;
import :project.index_store;
import :project.project;
import :sched.families.pch;
import :sched.families.pcm;
import :sched.families.turun;
import :sched.graph;
import :sched.index.pump;
import :worker.pool;

namespace clice {

/// The scheduling stack of one project, shared by the server and the batch
/// driver so the two assemble it the same way: the task graph with its
/// artifact families (runners registered) over the process's worker pool,
/// and the project's index store and pump, which a PCM landing that
/// unblocks indexing kicks. The server layers its serving side — the AST
/// family, the pump's escalation hook — on top.
struct SchedulingStack {
    SchedulingStack(kota::event_loop& loop,
                    Project& project,
                    CommandResolver& commands,
                    WorkerPool& pool);

    Project& project;
    WorkerPool& pool;
    TaskGraph graph;
    PCMFamily pcm;
    PCHFamily pch;
    IndexStore store;
    TURunFamily turun;
    IndexPump pump;

    /// The shutdown tail once compile and index work is quiesced
    /// (contract 11): wind down the graph's rounds, then the final save
    /// with the one metadata retry late debt may owe. The owner closes the
    /// cache store next (see close).
    kota::task<> shutdown();

    /// Close the cache store once no build of this stack is awaited: after
    /// the pool stopped, or right after shutdown() while the pool serves
    /// other projects — a worker still finishing a cancelled build then
    /// only fails its write.
    void close();
};

}  // namespace clice

module clice:sched.stack;

import :project.command_resolver;
import :project.project;
import :sched.builders.pch;
import :sched.builders.pcm;
import :sched.builders.turun;
import :sched.graph;
import :sched.index.pump;
import :support.prelude;
import :worker.pool;

namespace clice {

/// The scheduling stack of one project, shared by the server and the batch
/// driver so the two assemble it the same way: the task graph with its
/// artifact builders (runners registered) over the process's worker pool,
/// and the project's indexing pump, which a PCM commit that unblocks
/// indexing kicks. The server layers its serving side — the AST
/// family, the pump's escalation hook — on top.
struct SchedulingStack {
    SchedulingStack(kota::event_loop& loop,
                    Project& project,
                    CommandResolver& commands,
                    WorkerPool& pool);

    Project& project;
    WorkerPool& pool;
    TaskGraph graph;
    PCHBuilder pch;
    PCMBuilder pcm;
    TURunFamily turun;
    IndexPump pump;

    /// How often a long-lived owner calls checkpoint().
    static std::chrono::milliseconds checkpoint_interval();

    /// Persist the index and the cache store's manifest as they stand, a
    /// round running or not: a cold round covers the whole project, and
    /// a process killed mid-round loses only what merged since the last
    /// checkpoint.
    kota::task<> checkpoint();

    /// The shutdown tail once compile and index work is quiesced
    /// (contract 11): wind down the graph's rounds, then the final save.
    /// The owner closes the cache store next (see close).
    kota::task<> shutdown();

    /// Close the cache store once no build of this stack is awaited: after
    /// the pool stopped, or right after shutdown() while the pool serves
    /// other projects — a worker still finishing a cancelled build then
    /// only fails its write.
    void close();
};

}  // namespace clice

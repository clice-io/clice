module clice:project.indexer;

import :support.prelude;
import :support.signal;
import :vfs.ids;
import :worker.protocol;

namespace clice {

struct Project;

namespace testing {

struct IndexerFixture;

}

/// Why a file awaits re-indexing. The invalidation engine knows the cause
/// at enqueue time, so queries can decide in O(1) whether a pending file's
/// existing index is still trustworthy (see IndexQuery's freshness
/// contract).
enum class ReindexReason : std::uint8_t {
    /// Enqueued by a dependency cascade (or a bulk pass of unknown
    /// staleness): the file's own content is not known to have changed, so
    /// its index is positionally intact — at worst semantically
    /// behind — and keeps serving until the reindex finishes.
    DepsOnly,
    /// The file's own content changed: its index describes text that
    /// no longer exists, so queries skip this file's contribution until
    /// the reindex finishes.
    ContentChanged,
};

/// The background index's queue of the files owed a reindex: the order they
/// are indexed in, why each is owed, and the take/finish contract of an
/// attempt:
///
/// - record() books a pending reindex and queues the file unless it holds a
///   place not taken yet; within one queued place ContentChanged absorbs,
///   while a fresh place after consumption carries its own kind (the
///   consuming run owns the earlier one).
/// - A pass runs over the files queued when it began (begin_pass):
///   take_next() atomically takes the next file's pending reindex at
///   dispatch; events arriving during the flight book into the entry as a
///   newer one, and anything queued meanwhile waits for the next pass.
/// - finish() consumes exactly the taken reindex: a newer recording
///   survives an older attempt's completion.
/// - on_dispatch_failure() merges a failed taken reindex back into the
///   pending one (bounded for worker deaths by the requeue budget), unless
///   newer content already re-booked the work or the file's own run
///   crashed.
///
/// A TakenReindex carries the recording's sequence number as its identity;
/// the queue never touches sessions or workers.
class ReindexQueue {
public:
    struct TakenReindex {
        Fid id;
        std::uint64_t seq = 0;
    };

    /// Why a dispatch failed.
    enum class Failure : std::uint8_t {
        /// The run killed its worker (the worker named it): the same
        /// bytes would again, so the file waits for a change.
        Crashed,
        /// The worker died under it for some other reason — a death that
        /// named nothing, another request's crash: requeue on the budget.
        Lost,
        /// Preempted, or an outage the pool revives from: says nothing
        /// about the file, requeue freely.
        Preempted,
    };

    /// What a failed dispatch did to the file's pending state.
    enum class FailureDecision : std::uint8_t {
        /// No pending entry survived (file removed mid-flight).
        Dropped,
        /// The failed dispatch carried bytes older than the pending
        /// content; the newer content's own booking redoes the work.
        Outdated,
        /// The file's run crashed, or its requeue budget is spent; the
        /// file is abandoned and its pending state cleared.
        GaveUp,
        /// The taken reindex merged back into the pending one, queued for
        /// the next pass.
        Requeued,
    };

    explicit ReindexQueue(unsigned max_requeues) : max_requeues(max_requeues) {}

    /// Record (or refresh) why a file is pending, queueing it when it holds
    /// no place not taken yet. Every recording shields the entry from an
    /// in-flight attempt's finish; ContentChanged additionally starts a
    /// fresh crash budget — the crashes the old bytes caused say nothing
    /// about the fixed ones.
    void record(Fid id, ReindexReason reason);

    /// Move the file's place to the front of the places not taken yet. A
    /// place past the running pass stays past it and leads the next pass:
    /// the pass may have the file in flight, and a second attempt there
    /// would join the same node, where only one of the two commits a
    /// result.
    void move_to_front(Fid id);

    /// Start a pass over every file queued now, those `first` picks ahead
    /// of the others; how many places it has. Anything queued during the
    /// pass — its own failures' requeues included — waits for the next
    /// one: consuming a requeue in the pass that produced it is what let a
    /// worker outage spin the dispatch loop against instant failures (#611).
    std::size_t begin_pass(llvm::function_ref<bool(Fid)> first);

    /// Whether the running pass has places not taken yet.
    bool pass_left() const {
        return position < pass_end;
    }

    /// Take the next place of the running pass: its file's pending
    /// reindex. A place whose entry was cleared while it sat in the queue
    /// (the file was removed) is skipped; none when the pass has no place
    /// left.
    std::optional<TakenReindex> take_next();

    /// The running pass is over: its places not taken stay for the next
    /// one, and a queue whose every place was taken starts over empty.
    void end_pass();

    /// The places not taken yet.
    std::size_t queued() const {
        return order.size() - position;
    }

    /// The TakenReindex a dispatch of the file's pending reindex would
    /// take, with nothing consumed — for waiters binding to the pending
    /// attempt.
    std::optional<TakenReindex> peek(Fid id) const;

    /// Finish the taken reindex after its attempt: erases the entry unless
    /// a recording during the flight booked a newer one, which survives.
    void finish(const TakenReindex& taken);

    /// The taken reindex's result no longer commits: the entry was cleared,
    /// or a ContentChanged recording overtook the launch — its bytes
    /// describe text that no longer exists. A deps-only recording is
    /// deliberately NOT outdating it: the in-flight index is positionally
    /// right, and suppressing it would trade a tolerated semantic drift for
    /// a coverage hole.
    bool outdated(const TakenReindex& taken) const;

    /// Merge a failed taken reindex back into the pending one. A crash of
    /// the file's own abandons it; only lost runs spend the bounded budget
    /// — a preemption says nothing about the file, and capping it would
    /// silently drop coverage. The requeue carries the reason the dispatch
    /// was launched for, not the entry's current one: a deps-only
    /// downgrade during the flight bet on the content run finishing, and a
    /// failed run leaves the edit uncovered.
    FailureDecision on_dispatch_failure(const TakenReindex& taken, Failure failure);

    /// Why the file awaits re-indexing (queued or in flight), or nullopt
    /// when nothing is pending. O(1), no I/O.
    std::optional<ReindexReason> pending_reason(Fid id) const {
        auto it = entries.find(id);
        if(it == entries.end()) {
            return std::nullopt;
        }
        return it->second.reason;
    }

    bool contains(Fid id) const {
        return entries.count(id);
    }

    /// Forget a file's pending reindex (file removed from disk, or given
    /// up): nothing is left to reindex, and a lingering ContentChanged
    /// reason would suppress its deliberately still-serving shard forever.
    /// A place still in the queue stays: take_next skips it, and a file
    /// recorded again before then reuses it — a second place would dispatch
    /// the file twice in one pass.
    void clear(Fid id) {
        entries.erase(id);
    }

private:
    friend struct testing::IndexerFixture;

    struct Entry {
        ReindexReason reason;
        std::uint64_t seq;
        /// The sequence number of the newest ContentChanged recording.
        /// outdated() compares against this, not `seq`: a deps-only requeue
        /// during a flight bumps `seq` (to survive the finish) but must not
        /// discard an in-flight content run.
        std::uint64_t content_seq;

        /// Lost-run requeues of this entry. Bounds the damage of a file
        /// whose runs keep dying without the worker naming it (an OOM
        /// kill): without a cap, every requeue would burn another worker.
        unsigned requeues = 0;
    };

    llvm::DenseMap<Fid, Entry> entries;

    /// The files in the order they are indexed; those before `position`
    /// are taken.
    std::vector<Fid> order;

    std::size_t position = 0;

    /// Where the running pass's places end; `position` once it is over.
    std::size_t pass_end = 0;

    /// The files holding a place not taken yet, for record()'s fresh-place
    /// decision.
    llvm::DenseSet<Fid> queued_files;

    std::uint64_t seq = 0;

    unsigned max_requeues;
};

/// The background index of a project: the passes that drive its units
/// through their compiles (Project::compile_unit) to drain the queue of owed
/// reindexes, batched by an idle delay and fed to the workers within a
/// window — and nothing about what a run produces or where results live
/// (the project's index). Reindexes are booked whether it runs or not;
/// passes run once its owner starts it: a server that indexes when its
/// project opened, `clice index` at once, `clice lint --index` after its
/// own pass.
///
/// It is serving-neutral: what an open document changes about a file's run
/// it asks the project's serving side (Project::serving), which a
/// command-line run has none of.
class BackgroundIndex {
public:
    explicit BackgroundIndex(Project& project) : project(project) {}

    /// Progress of the current (or last) indexing pass. The reporter reads
    /// it on each on_progress_changed emission — the signal only wakes it.
    struct Progress {
        enum class Stage : std::uint8_t { Begin, Report, End };

        Stage stage = Stage::Begin;

        std::size_t total = 0;

        /// The pass's units finished: indexed, skipped as fresh, failed,
        /// or refused by the graph.
        std::size_t completed = 0;

        std::size_t dispatched = 0;
    };

    /// What a file's latest attempt left when it did not index clean: an
    /// index with the errors its compile reported, or a failure for good —
    /// rejected by the worker, an empty or unverifiable result, a spent
    /// crash budget, or a dead IPC path — for want of memory or not. A
    /// later attempt that indexes clean removes it again. The one-shot
    /// `clice index` reports a partial build from these.
    struct LastAttempt {
        bool failed = false;

        /// A failure for want of memory alone: what its worker reached.
        std::optional<std::uint64_t> too_big;

        /// The errors an indexed compile reported (start's `errors`).
        std::vector<worker::LocatedMessage> errors;
    };

    /// Passes run from now on: what was booked so far, and what the index
    /// owes — every member it holds nothing of and every unit outdated
    /// (ProjectIndex::mark_outdated), DepsOnly, so that a cold start with a
    /// warm index keeps serving the stored shards instead of blanking every
    /// query until the pass drains, and the units owed the variants a
    /// rebuild discarded, read fresh by content. With `errors`, every
    /// attempt brings back the errors its compile reported, for
    /// last_attempts: the one-shot `clice index` lists them; a server
    /// reports none and never asks.
    void start(bool errors = false);

    /// Passes run: started and not stopped.
    bool started() const {
        return state == State::Started;
    }

    /// Cancel background indexing and wait for all its tasks to finish; no
    /// pass starts again.
    async::task<> stop();

    /// Book a reindex of the file. A file booked twice keeps a single queue
    /// entry; its reason is upgraded to ContentChanged if either booking
    /// says so (a file both cascaded onto and edited is as stale as the
    /// edit makes it). False for a file the build does not compile as a
    /// unit, or one a rule keeps out of the index: nothing is booked and no
    /// attempt will ever finish.
    bool enqueue(Fid file, ReindexReason reason);

    /// Someone is reading the file through the index and nothing serves it
    /// yet: book it at the front of the queue and start a pass without the
    /// idle delay. A running pass finishes undisturbed; the file then leads
    /// the next one.
    void index_first(Fid file);

    /// Forget a file's pending reindex: used when the file is removed from
    /// disk — nothing is left to reindex, and a lingering ContentChanged
    /// reason would suppress its deliberately still-serving shard forever.
    /// Its attempt's waiters wake.
    void forget(Fid file);

    /// The reindexes booked and not finished.
    const ReindexQueue& queue() const {
        return reindexes;
    }

    /// Schedule a pass (respects the idle delay and dedup). `immediate`
    /// skips the idle batching window — used by the pass tail for work
    /// requeued during the pass that just ended, and by index_first, which
    /// re-arms an already-armed timer to fire now (waiting out a previously
    /// scheduled idle window would starve the reader). Nothing before
    /// start(): what is booked waits for it.
    void schedule(bool immediate = false);

    /// No pass runs, and none is owed: nothing is queued, or it never runs.
    bool idle() const {
        return !active && (reindexes.queued() == 0 || !started());
    }

    /// Wait until idle(): every pass, requeue passes included, has run and
    /// saved its results.
    async::task<> wait_idle();

    /// Wait until the pending (re)index attempt observed at call time finishes
    /// — the merge committed, the attempt gave up, or the entry was cleared —
    /// and return immediately when nothing is pending. One attempt, not an
    /// idle file: a requeue arriving during the flight does not extend the
    /// wait. For replies clients cache with no refresh request (outline,
    /// links): answered while the didOpen index_first is still running, the
    /// empty result would freeze until the next edit.
    async::task<> await_attempt(Fid file);

    /// Temporarily pause background indexing to give priority to user
    /// requests. Indexing tasks already dispatched to workers continue, but no
    /// new tasks are sent until resume() is called. Nested pairs work.
    void pause();

    void resume();

    /// Pause once `units` more files are dispatched, so exactly that many
    /// finish before the pause holds (CLICE_TEST_PAUSE).
    void pause_after(std::size_t units) {
        dispatches_before_pause = units;
    }

    const Progress& progress() const {
        return current;
    }

    /// Emitted whenever the indexing progress changes (pass begins, a file
    /// completes, pass ends).
    Signal<> on_progress_changed;

    /// The files whose latest attempt did not index clean.
    const llvm::DenseMap<Fid, LastAttempt>& last_attempts() const {
        return attempts;
    }

    /// Whether the file's latest attempt failed for good.
    bool failed(Fid file) const {
        auto it = attempts.find(file);
        return it != attempts.end() && it->second.failed;
    }

    /// TUs actually indexed and merged this session — dispatches the
    /// freshness gate skipped do not count. The one-shot `clice index`
    /// reports its work from this, so a warm rerun honestly says 0.
    std::size_t indexed() const {
        return indexed_units;
    }

private:
    friend struct testing::IndexerFixture;

    struct PassState;

    /// One pass, once the idle delay schedule() armed has passed: index the
    /// files queued when it starts, save, and schedule the next pass for
    /// what it queued.
    async::task<> run_pass();

    async::task<> feed(async::task_group<>& workers,
                       PassState& pass,
                       std::size_t total,
                       std::size_t& dispatched);

    async::task<> run(ReindexQueue::TakenReindex taken,
                      std::size_t index,
                      std::size_t total,
                      PassState& pass);

    /// Wake the file's waiters whose observed sequence number the finished
    /// attempt covers (`seq` and older) and drop their events. The default
    /// wakes every waiter — for the paths where no further attempt will come
    /// (entry cleared, shutdown).
    void wake_waiters(Fid file, std::uint64_t seq = -1);

    void record(Fid file, LastAttempt attempt);

private:
    constexpr static unsigned max_requeues = 3;

    Project& project;

    enum class State : std::uint8_t { Off, Started, Stopped };

    State state = State::Off;

    bool errors = false;

    /// The pending reindexes and their order. The rules on top of it, each
    /// born from a concrete bug:
    /// 1. The freshness check inside the index task is the ONLY place
    ///    that decides to skip work. Duplicating it at the feeder
    ///    reintroduces reason-blind skips.
    /// 2. needs_reindex() may shortcut deps-only reindexes ONLY: whoever booked
    ///    a content change saw the edit itself, and the dep-hash check
    ///    cannot see a file's own edit.
    ReindexQueue reindexes{max_requeues};

    Progress current;

    llvm::DenseMap<Fid, LastAttempt> attempts;

    std::size_t indexed_units = 0;

    struct AttemptWait {
        std::uint64_t seq;

        std::shared_ptr<async::event> event;
    };

    /// The events await_attempt waiters park on, per file and per pending
    /// sequence number observed at wait time; each fires when an attempt
    /// covering it finishes (or wholesale at stop). Keyed by it so a
    /// requeue during an attempt's flight cannot extend earlier waits — the
    /// wait promises one attempt, not an idle file.
    llvm::DenseMap<Fid, llvm::SmallVector<AttemptWait, 2>> attempt_waits;

    /// A pass runs, through its save.
    bool active = false;

    /// A pass waits on the idle timer.
    bool scheduled = false;

    /// Set whenever a pass ends; wait_idle() waits on it.
    async::event pass_ended{false};

    std::shared_ptr<async::timer> idle_timer;

    /// When paused, new index tasks wait on this event. A counter, so
    /// nested pause/resume pairs work.
    std::size_t pause_depth = 0;

    async::event resume_event{true};

    /// Dispatches left before pause_after() pauses; zero when none is due.
    std::size_t dispatches_before_pause = 0;

    async::task_group<> tasks;
};

/// Pauses indexing for its lifetime; a moved-from one holds nothing.
struct [[nodiscard]] ScopedPause {
    BackgroundIndex* background;

    explicit ScopedPause(BackgroundIndex& background) : background(&background) {
        background.pause();
    }

    ScopedPause(ScopedPause&& other) noexcept :
        background(std::exchange(other.background, nullptr)) {}

    ScopedPause& operator=(ScopedPause&&) = delete;

    ~ScopedPause() {
        if(background) {
            background->resume();
        }
    }
};

}  // namespace clice

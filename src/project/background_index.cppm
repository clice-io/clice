module clice:project.background_index;

import :feature.diagnostics;
import :index.project_index;
import :project.reindex_queue;
import :support.prelude;
import :support.signal;
import :worker.protocol;

namespace clice {

struct Project;

/// The background index's state: the standing-interest scheduler that
/// drives the build's units through unit runs, by its queue, the idle
/// batching timer and the feeder window — and nothing about what a run
/// produces (unit_run) or where results live (the project's index).
///
/// It is serving-neutral: it never sees the open documents. The server
/// injects its policy through `compiled_by_session` and
/// `on_attempt_finished`; a command-line run installs neither.
struct BackgroundIndex {
    constexpr static unsigned max_requeue_attempts = 3;

    /// Invoked when an index attempt finished with no retry pending, before
    /// the attempt's waiters wake (contract 15): the serving side decides
    /// whether the finished state can ever serve an open session and
    /// escalates it otherwise, so the waking waiters re-derive their route
    /// against the escalated state. Also invoked for an index_first the
    /// background index refuses — a file a rule keeps out of the index —
    /// since no attempt will ever finish for it.
    std::function<void(Fid path_id)> on_attempt_finished;

    /// Whether an open document's own compile serves the file: a pending
    /// reindex other than a change of its own content then is cleared
    /// without a background compile, which would compile the file a second
    /// time. The serving side enqueues the file again when the document
    /// closes.
    std::function<bool(Fid path_id)> compiled_by_session;

    /// Under `skip_header_bodies`, every index run also brings back the
    /// diagnostics an editor can get wrong on its file
    /// (UnitRunPlan::skipped_diagnostics), handed here with the hash of the
    /// text they describe — unless a reindex booked during the run says
    /// they describe inputs that changed.
    std::function<void(Fid path_id,
                       std::uint64_t text_hash,
                       std::vector<feature::FileDiagnostic> diagnostics)>
        on_skipped_diagnostics;

    /// Whether the serving side lacks those diagnostics for the file: its
    /// run goes ahead even when its index is current.
    std::function<bool(Fid path_id)> lacks_skipped_diagnostics;

    /// Emitted when shards that may be index-served changed (merged,
    /// re-masked, dropped or shed). Carries the affected path_ids; the
    /// serving adapter checks which of them an open session serves and
    /// refreshes those clients.
    Signal<llvm::ArrayRef<Fid>> on_index_changed;

    /// Progress of the current (or last) indexing round. The reporter reads
    /// it on each on_progress_changed emission — the signal only wakes it.
    struct Progress {
        enum class Stage : std::uint8_t { Begin, Report, End };

        Stage stage = Stage::Begin;

        std::size_t total = 0;

        /// The round's units finished: indexed, skipped as fresh, failed,
        /// or refused by the graph.
        std::size_t completed = 0;

        std::size_t dispatched = 0;
    };

    Progress progress;

    /// Emitted whenever the indexing progress changes (round begins, a file
    /// completes, round ends).
    Signal<> on_progress_changed;

    /// The pending reindexes and their order. The rules on top of it, each
    /// born from a concrete bug:
    /// 1. The freshness check inside the index task is the ONLY place
    ///    that decides to skip work. Duplicating it at the feeder
    ///    reintroduces reason-blind skips.
    /// 2. needs_reindex() may shortcut deps-only slots ONLY: the engine
    ///    observed content changes itself, and the dep-hash check cannot
    ///    see a file's own edit.
    ReindexQueue queue{max_requeue_attempts};

    /// What a file's latest attempt left, kept by record_* in
    /// background_index.cpp alone: an index, with the errors its compile
    /// reported, or a failure for good — rejected by the worker, an empty
    /// or unverifiable result, a spent crash budget, or a dead IPC path —
    /// for want of memory or not. A file is in at most one of `failed` and
    /// `errors`; `too_big` holds failed ones, with the memory their worker
    /// reached alone. A later successful pass removes them again. The
    /// one-shot `clice index` reports a partial build from these.
    llvm::DenseSet<Fid> failed;

    llvm::DenseMap<Fid, std::uint64_t> too_big;

    llvm::DenseMap<Fid, std::vector<worker::LocatedMessage>> errors;

    /// Every index attempt brings back the errors its compile reported, for
    /// `errors`. The one-shot `clice index` lists them; a server reports
    /// none and never asks.
    bool errors_wanted = false;

    /// TUs actually indexed and merged this session — dispatches the
    /// freshness gate skipped do not count. The one-shot `clice index`
    /// reports its work from this, so a warm rerun honestly says 0.
    std::size_t indexed = 0;

    struct AttemptWait {
        std::uint64_t seq;

        std::shared_ptr<async::event> event;
    };

    /// The events await_index_attempt waiters park on, per file and per
    /// pending sequence number observed at wait time; each fires when an
    /// attempt covering it finishes (or wholesale at stop). Keyed by it so a
    /// requeue during an attempt's flight cannot extend earlier waits — the
    /// wait promises one attempt, not an idle file.
    llvm::DenseMap<Fid, llvm::SmallVector<AttemptWait, 2>> attempt_waits;

    /// A round runs, through its save.
    bool active = false;

    /// A round waits on the idle timer.
    bool scheduled = false;

    /// Set whenever a round ends; wait_index_idle() waits on it.
    async::event round_ended{false};

    std::shared_ptr<async::timer> idle_timer;

    /// Pause/resume: when paused, new index tasks wait on this event. A
    /// counter, so nested pause/resume pairs work.
    std::size_t pause_depth = 0;

    async::event resume_event{true};

    /// Dispatches left before pause_indexing_after() pauses; zero when none
    /// is due.
    std::size_t dispatches_before_pause = 0;

    /// stop_background_index() ran: no round starts again.
    bool stopped = false;

    async::task_group<> tasks;
};

/// Add a file to the background indexing queue. A file enqueued twice
/// keeps a single queue entry; its reason is upgraded to ContentChanged if
/// either enqueue says so (a file both cascaded onto and edited is as stale
/// as the edit makes it). False when a rule keeps the file out of the
/// index: nothing is queued and no attempt will ever finish.
bool enqueue_reindex(Project& project, Fid path_id, ReindexReason reason);

/// Someone is reading the file through the index and nothing serves it
/// yet: enqueue it at the front of the queue and start a round without the
/// idle delay. A running round finishes undisturbed; the file then leads
/// the next one.
void index_first(Project& project, Fid path_id);

/// Wait until the pending (re)index attempt observed at call time finishes
/// — the merge committed, the attempt gave up, or the entry was cleared —
/// and return immediately when nothing is pending. One attempt, not an
/// idle file: a requeue arriving during the flight does not extend the
/// wait. For replies clients cache with no refresh request (outline,
/// links): answered while the didOpen index_first is still running, the
/// empty result would freeze until the next edit.
async::task<> await_index_attempt(Project& project, Fid path_id);

/// Wake the file's waiters whose observed sequence number the finished
/// attempt covers (`seq` and older) and drop their events. The default
/// wakes every waiter — for the paths where no further attempt will come
/// (entry cleared, shutdown).
void wake_attempt_waits(Project& project, Fid path_id, std::uint64_t seq = -1);

/// Forget a file's pending reindex: used when the file is removed from
/// disk — nothing is left to reindex, and a lingering ContentChanged reason
/// would suppress its deliberately still-serving shard forever. Its
/// attempt's waiters wake.
void clear_reindex(Project& project, Fid path_id);

/// Schedule background indexing (respects idle timeout and dedup).
/// `immediate` skips the idle batching window — used by the round tail for
/// work requeued during the round that just ended, after a save that
/// queued reindexes, and by index_first, which re-arms an already-armed
/// timer to fire now (waiting out a previously scheduled idle window would
/// starve the reader).
void schedule_index(Project& project, bool immediate = false);

/// One round now, without the idle delay: index the files queued when it
/// starts, save, and schedule the next round for what it queued.
async::task<> run_index_round(Project& project);

/// At the start of a run writing the index: queue every member the index
/// holds nothing of and every unit depending on a file that changed while
/// no server ran, DepsOnly — a cold start with a warm index keeps serving
/// the stored shards, not blanking every query until the pass drains — and
/// the units owed the variants a rebuild discarded, read fresh by content;
/// then schedule. With indexing off it queues nothing, but the units whose
/// runs walked a changed file are still marked outdated.
void enqueue_outdated(Project& project, llvm::ArrayRef<Fid> members);

/// Take an index report: book its reindexes — before the current attempt
/// finishes and its waiters wake, so a waker never observes an index as
/// current that the report just declared stale — and report its
/// serving-index changes through on_index_changed.
void apply_report(Project& project, const index::IndexReport& report);

/// The files a save records as owed an index: the failed ones and every
/// pending reindex.
llvm::SmallVector<Fid> save_debt(const Project& project);

/// Save the index and take the report, again while a save owes a retry (a
/// grown map can still be too small); an idle save then shrinks the
/// database file when that is worth it (shrink_database). Reindexes the
/// saves queued start a round when none runs.
async::task<> persist(Project& project, index::SearchRebuild search = index::SearchRebuild::Behind);

/// Cancel background indexing and wait for all its tasks to finish.
async::task<> stop_background_index(Project& project);

/// No round runs and nothing is queued.
bool index_idle(const Project& project);

/// Wait until index_idle(): every round, requeue rounds included, has run
/// and saved its results.
async::task<> wait_index_idle(Project& project);

/// Temporarily pause background indexing to give priority to user
/// requests. Indexing tasks already dispatched to workers continue, but no
/// new tasks are sent until resume_indexing() is called.
void pause_indexing(Project& project);

/// Resume background indexing after a pause.
void resume_indexing(Project& project);

/// Pause indexing once `units` more files are dispatched, so exactly that
/// many finish before the pause holds (CLICE_TEST_PAUSE).
void pause_indexing_after(Project& project, std::size_t units);

/// Pauses indexing for its lifetime; a moved-from one holds nothing.
struct [[nodiscard]] ScopedPause {
    Project* project;

    explicit ScopedPause(Project& project) : project(&project) {
        pause_indexing(project);
    }

    ScopedPause(ScopedPause&& other) noexcept : project(std::exchange(other.project, nullptr)) {}

    ScopedPause& operator=(ScopedPause&&) = delete;

    ~ScopedPause() {
        if(project) {
            resume_indexing(*project);
        }
    }
};

/// How often a long-lived owner calls Project::checkpoint().
chrono::milliseconds checkpoint_interval();

}  // namespace clice

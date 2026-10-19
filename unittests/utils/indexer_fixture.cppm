module;

#include "modules/prelude.h"

module clice:unittests.utils.indexer_fixture;

import :command.argument_parser;
import :compile.compilation;
import :index.database;
import :index.indexer;
import :index.project_index;
import :index.tu_index;
import :project.command_resolver;
import :project.load;
import :project.project;
import :project.save;
import :sched.families.pch;
import :sched.families.pcm;
import :sched.families.turun;
import :sched.graph;
import :sched.index.pending_table;
import :sched.index.pump;
import :unittests.utils.temp_dir;
import :unittests.utils;
import :vfs.cache_store;
import :vfs.file_table;
import :vfs.ids;
import :vfs.path;
import :worker.pool;

namespace clice::testing {

/// Test fixture with friend access to the pump's internals. The take
/// wrappers (merge/save/load/drop_index) route each index report back into
/// the pump, as the production callers do.
struct IndexerFixture {
    using Decision = PendingTable::FailureDecision;

    constexpr static unsigned budget = IndexPump::max_requeue_attempts;

    kota::event_loop loop;
    FileTable files;
    Project project{files};
    WorkerPool pool{loop};
    CommandResolver commands{project};
    TaskGraph graph;
    PCMFamily pcm{graph, project, commands, pool};
    PCHFamily pch{graph, project, pool};
    TURunFamily turun{graph, project, commands, pcm, pch, pool};
    IndexPump pump{loop, project, commands, turun, pool};

    IndexerFixture() {
        turun.register_runner();
    }

    /// A read that found lost data scheduled a round the test never ran:
    /// stopped, its frame does not outlive the loop.
    ~IndexerFixture() {
        if(pump.indexing_scheduled) {
            stop_pump();
        }
    }

    /// Stop the pump: a round a read scheduled never runs, and the test
    /// takes the lost data itself when it chooses (take_lost).
    void stop_pump() {
        auto task = pump.stop();
        loop.schedule(task);
        loop.run();
    }

    /// Merge a worker result and take its report, as the TURun round does.
    bool merge(llvm::StringRef data) {
        auto report = project.index.merge(data, {});
        if(report) {
            pump.apply_report(*report);
        }
        return report.has_value();
    }

    /// One save with the pump's debt snapshot, its report taken back —
    /// without persist()'s retry, so a test observes each save.
    kota::task<> async_save(index::SearchRebuild search = index::SearchRebuild::Behind) {
        pump.apply_report(co_await save_project(project, commands, loop, pump.save_debt(), search));
    }

    /// Load and take the report, as the workspace load does. Returns the
    /// decode result (false = old-format or corrupt project table).
    bool load(bool read_only = false) {
        auto result = load_saved(project, commands, {.read_only = read_only});
        pump.apply_report(result.report);
        return result.decoded;
    }

    /// Whether a background round is scheduled and not yet started.
    bool round_scheduled() const {
        return pump.indexing_scheduled;
    }

    /// Take the lost data reads found to the pump, as a round does.
    void take_lost() {
        pump.apply_report(project.index.take_lost());
    }

    void drop_index(Fid id) {
        index::IndexReport report;
        project.index.drop(id, report);
        pump.apply_report(report);
    }

    /// Replace the database with a fresh one, as corruption recovery does.
    void reopen_database() {
        index::IndexReport report;
        project.index.recover_corrupt_database(report);
        reopen_fresh_database(project);
        pump.apply_report(report);
    }

    /// Record a terminally failed attempt, as run_index_task's failure
    /// decisions do.
    void mark_failed(Fid id) {
        pump.failed_ids.insert(id);
    }

    /// Fail the entry's current dispatch: the launch ticket matches.
    Decision fail(Fid id, PendingTable::Failure failure) {
        return pump.note_dispatch_failure({id, ticket(id)}, failure);
    }

    /// Fail a dispatch launched with an explicit (possibly stale) ticket.
    Decision fail_at(Fid id, std::uint64_t ticket, PendingTable::Failure failure) {
        return pump.note_dispatch_failure({id, ticket}, failure);
    }

    std::uint64_t ticket(Fid id) {
        auto it = pump.pending_table.entries.find(id);
        return it == pump.pending_table.entries.end() ? std::numeric_limits<std::uint64_t>::max()
                                                      : it->second.ticket;
    }

    unsigned attempts(Fid id) {
        auto it = pump.pending_table.entries.find(id);
        return it == pump.pending_table.entries.end() ? 0u : it->second.requeue_attempts;
    }

    void set_attempts(Fid id, unsigned n) {
        pump.pending_table.entries.find(id)->second.requeue_attempts = n;
    }

    /// Consume the queued slot as a dispatch would, so a later enqueue
    /// takes the fresh-slot (mid-flight) path.
    void consume(Fid id) {
        pump.pending_table.queued.erase(id);
    }

    /// Complete an attempt for `ticket` on the loop, as run_index_task's
    /// tail does — waking waiters requires a running loop.
    void wake_waiters(Fid id, std::uint64_t ticket) {
        auto body = [&]() -> kota::task<> {
            pump.wake_attempt_waits(id, ticket);
            co_return;
        };
        auto task = body();
        loop.schedule(task);
        loop.run();
    }

    /// Start a fresh staleness round: per-round FileVersion results are
    /// cleared by run_background_indexing, which these tests bypass.
    void begin_round() {
        project.index.begin_round();
    }

    /// The instantiation keys the worker indexing `tu` may skip, as the
    /// TURun round asks for them.
    std::vector<std::uint64_t> known_instantiations(Fid tu) {
        return project.index.known_instantiations(tu, project.dep_graph.include_closure(tu));
    }

    /// Judge staleness inside the current round (see begin_round).
    bool needs_reindex(llvm::StringRef path) {
        return project.index.needs_reindex(project.file_table.intern(Spelling::absolute(path)));
    }

    /// Record a standalone header's borrowed host, as the TURun round does
    /// after its merge commits (these tests merge worker results directly).
    void set_header_host(Fid header_id, Fid host_id) {
        project.command_snapshot.record_host(header_id, host_id);
    }

    /// Run one save() to completion on the fixture's loop.
    void save(index::SearchRebuild search = index::SearchRebuild::Behind) {
        auto task = async_save(search);
        loop.schedule(task);
        loop.run();
    }

    /// One background round as a schedulable task, for tests that need to
    /// interleave other tasks with it.
    kota::task<> round_task() {
        return pump.run_background_indexing();
    }

    /// Run one background round to completion on the fixture's loop.
    void run_round() {
        auto task = round_task();
        loop.schedule(task);
        loop.run();
    }
};

/// The TUs that contributed to `file`, each with its variant.
inline llvm::SmallDenseMap<Fid, std::uint64_t, 2> contributions_of(const index::ProjectIndex& index,
                                                                   Fid file) {
    llvm::SmallDenseMap<Fid, std::uint64_t, 2> contributions;
    index.for_each_contribution(file, [&](Fid tu, std::uint64_t variant) {
        contributions[tu] = variant;
    });
    return contributions;
}

struct IndexedTU {
    /// TU index bytes, as a worker would ship them.
    std::string data;

    /// The TU's canonical path inside the index.
    std::string tu_path;
};

/// Index a real on-disk file in-process into its TU index bytes, the
/// sections of `known` variants left empty and the `known_instantiations`
/// not walked.
inline IndexedTU index_file(TempDir& tmp,
                            llvm::StringRef file,
                            std::vector<std::string> extra_args = {},
                            llvm::ArrayRef<std::uint64_t> known = {},
                            llvm::ArrayRef<std::uint64_t> known_instantiations = {}) {
    std::string resource = std::string(resource_dir());
    std::vector<std::string> args =
        {"clang++", "-fsyntax-only", "-resource-dir", resource, "-c", std::string(file)};
    args.insert(args.end(), extra_args.begin(), extra_args.end());

    CompilationParams cp;
    cp.kind = CompilationKind::Whole;
    cp.input.directory = std::string(tmp.root);
    cp.input.arguments = std::move(args);

    auto unit = compile(std::move(cp));
    if(!unit->completed()) {
        return {};
    }
    IndexedTU result;
    result.data = index::build_tu_index(
        *unit,
        {.known_variants = known, .known_instantiations = known_instantiations});
    auto view = index::TUIndex::from_bytes(result.data);
    if(!view.loaded()) {
        return {};
    }
    result.tu_path = std::string(view.path(view.path_count() - 1));
    return result;
}

inline void open_store(TempDir& tmp, Project& project) {
    auto store = CacheStore::open(tmp.path("cache"), 1);
    ZASSERT(store);
    project.store.emplace(std::move(*store));
    project.database = index::open_lmdb_database(*project.store, "");
    project.index.attach(project.database.get());
}

/// Plant a blob between sessions (no fixture may be alive — the writer
/// lock), as the residue of a crash or a foreign writer.
inline void
    inject_blob(TempDir& tmp, index::BlobKind kind, llvm::StringRef key, std::string bytes) {
    auto store = CacheStore::open(tmp.path("cache"), 1);
    ZASSERT(store);
    auto db = index::open_lmdb_database(*store, "");
    ZASSERT(db != nullptr);
    index::BlobDatabase::Blob blob{kind, key.str(), std::move(bytes)};
    ZASSERT(db->write(blob, {}).empty());
}

}  // namespace clice::testing

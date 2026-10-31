module clice:unittests.utils.indexer_fixture;

import :command.table;
import :compile.compilation;
import :index.database;
import :index.indexer;
import :index.project_index;
import :index.tu_index;
import :project.indexer;
import :project.project;
import :support.bytes;
import :support.prelude;
import :unittests.utils.cdb_helper;
import :unittests.utils.temp_dir;
import :unittests.utils;
import :vfs.cache_store;
import :vfs.file_table;
import :vfs.ids;
import :vfs.path;
import :worker.pool;

namespace clice::testing {

/// Test fixture with friend access to the reindex queue's internals. The
/// take wrappers (merge/save/load/drop_index) route each index report back
/// into the background index, as the production callers do.
struct IndexerFixture {
    using Decision = ReindexQueue::FailureDecision;

    constexpr static unsigned budget = BackgroundIndex::max_requeues;

    async::event_loop loop;
    FileTable files;
    WorkerPool pool{loop};
    Project project{files, {}, &loop, &pool};
    BackgroundIndex& background = project.background_index;
    ReindexQueue& queue = project.background_index.queue;

    struct Unit {
        std::string name;
        std::vector<std::string> extra_args;
    };

    /// List `units` — files of `tmp`, each with the extra arguments it is
    /// indexed with — in the project's compilation database, as the build
    /// lists the units a test indexes: a load keeps a unit's index only
    /// while the build compiles it.
    void list_units(TempDir& tmp, llvm::ArrayRef<Unit> units) {
        std::vector<CDBEntry> entries;
        for(auto& unit: units) {
            entries.push_back({tmp.root, tmp.path(unit.name), unit.extra_args});
        }
        write_cdb(tmp, project.cdb, build_cdb_json(entries));
    }

    /// Merge a worker result under the command the build gives its unit and
    /// take its report, as the unit run does.
    bool merge(llvm::StringRef data) {
        std::uint64_t command = 0;
        if(auto view = index::TUIndex::from_bytes(data); view.loaded()) {
            auto unit = files.intern(Spelling::absolute(view.path(view.focus_file())));
            command = project.command_identity(unit);
        }
        auto report = project.index.merge(data, {}, command);
        if(report) {
            project.apply_report(*report);
        }
        return report.has_value();
    }

    /// One save, its report taken back —
    /// without Project::save's retry, so a test observes each save.
    async::task<> async_save(index::SearchRebuild search = index::SearchRebuild::Behind) {
        project.apply_report(co_await project.write_database(search));
    }

    /// Load and take the report, as the project's opening does, reconciled
    /// with the build on a writable database. Returns the decode result
    /// (false = a header of another format, or corrupt).
    bool load() {
        auto result = project.load_saved(project.database && !project.database->read_only());
        project.apply_report(result.report);
        return result.decoded;
    }

    void drop_index(Fid id) {
        index::IndexReport report;
        project.index.drop(id, report);
        project.apply_report(report);
    }

    /// Replace the database with a fresh one, as corruption recovery does.
    void reopen_database() {
        project.reopen_fresh_database();
    }

    /// Record a terminally failed attempt, as the index task's failure
    /// decisions do.
    void mark_failed(Fid id) {
        background.failed.insert(id);
    }

    /// Fail the entry's current dispatch: the launch sequence number matches.
    Decision fail(Fid id, ReindexQueue::Failure failure) {
        return queue.on_dispatch_failure({id, seq(id)}, failure);
    }

    /// Fail a dispatch launched with an explicit (possibly stale) sequence number.
    Decision fail_at(Fid id, std::uint64_t seq, ReindexQueue::Failure failure) {
        return queue.on_dispatch_failure({id, seq}, failure);
    }

    /// Every file `queue` holds a pending reindex for, sorted.
    static llvm::SmallVector<Fid> pending_files(const ReindexQueue& queue) {
        auto files = llvm::to_vector(llvm::make_first_range(queue.entries));
        ranges::sort(files);
        return files;
    }

    std::uint64_t seq(Fid id) {
        auto it = queue.entries.find(id);
        return it == queue.entries.end() ? std::numeric_limits<std::uint64_t>::max()
                                         : it->second.seq;
    }

    unsigned requeues(Fid id) {
        auto it = queue.entries.find(id);
        return it == queue.entries.end() ? 0u : it->second.requeues;
    }

    void set_requeues(Fid id, unsigned n) {
        queue.entries.find(id)->second.requeues = n;
    }

    /// Consume the queued place as a dispatch would, so a later enqueue
    /// takes the fresh-place (mid-flight) path.
    void consume(Fid id) {
        queue.queued_files.erase(id);
    }

    /// Complete an attempt for `seq` on the loop, as the index task's tail
    /// does — waking waiters requires a running loop.
    void wake_waiters(Fid id, std::uint64_t seq) {
        auto body = [&]() -> async::task<> {
            project.wake_attempt_waits(id, seq);
            co_return;
        };
        auto task = body();
        loop.schedule(task);
        loop.run();
    }

    /// Clear the staleness cache: per-pass FileVersion results are
    /// cleared by run_index_pass, which these tests bypass.
    void clear_staleness_cache() {
        project.index.clear_staleness_cache();
    }

    /// The instantiation keys the worker indexing `tu` may skip, as the
    /// unit run asks for them.
    std::vector<std::uint64_t> known_instantiations(Fid tu) {
        return project.index.known_instantiations(tu, project.dep_graph.include_closure(tu));
    }

    /// Judge staleness inside the current pass (see clear_staleness_cache).
    bool needs_reindex(llvm::StringRef path) {
        return project.index.needs_reindex(project.file_table.intern(Spelling::absolute(path)));
    }

    /// Run one save() to completion on the fixture's loop.
    void save(index::SearchRebuild search = index::SearchRebuild::Behind) {
        auto task = async_save(search);
        loop.schedule(task);
        loop.run();
    }

    /// One background pass as a schedulable task, for tests that need to
    /// interleave other tasks with it.
    async::task<> pass_task() {
        return project.run_index_pass();
    }

    /// Run one background pass to completion on the fixture's loop.
    void run_pass() {
        auto task = pass_task();
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
    result.data = std::string(as_chars(index::build_tu_index(
        *unit,
        {.known_variants = known, .known_instantiations = known_instantiations})));
    auto view = index::TUIndex::from_bytes(result.data);
    if(!view.loaded()) {
        return {};
    }
    result.tu_path = std::string(view.path(view.focus_file()));
    return result;
}

inline void open_store(TempDir& tmp, Project& project) {
    auto store = CacheStore::open(tmp.path("cache"), 1);
    ZASSERT(store);
    project.store.emplace(std::move(*store));
    project.database = index::open_lmdb_database(*project.store, {});
}

/// Plant a blob between sessions (no fixture may be alive — the writer
/// lock), as the residue of a crash or a foreign writer.
inline void
    inject_blob(TempDir& tmp, index::BlobKind kind, llvm::StringRef key, llvm::StringRef bytes) {
    auto store = CacheStore::open(tmp.path("cache"), 1);
    ZASSERT(store);
    auto db = index::open_lmdb_database(*store, {});
    ZASSERT(db != nullptr);
    index::BlobDatabase::Blob blob{kind, key.str(), copy_bytes(bytes)};
    ZASSERT(db->write(blob, {}));
}

}  // namespace clice::testing

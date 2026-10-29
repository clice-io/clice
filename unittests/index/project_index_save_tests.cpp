module clice;

import :index.database;
import :index.project_index;
import :index.shard;
import :index.symbol;
import :index.symbol_query;
import :project.command_resolver;
import :project.project;
import :project.save;
import :sched.index.pending_table;
import :sched.index.pump;
import :sched.stack;
import :support.bytes;
import :unittests.utils.forwarding_database;
import :unittests.utils.index_internals;
import :unittests.utils.indexer_fixture;
import :unittests.utils.temp_dir;
import :unittests.utils;
import :vfs.cache_store;
import :vfs.file_table;
import :vfs.ids;
import :vfs.path;
import :worker.pool;

namespace clice::testing {
namespace {

/// Forwards to a real database and logs the blob kinds of each write.
struct WriteLog final : ForwardingDatabase {
    using ForwardingDatabase::ForwardingDatabase;

    std::vector<std::vector<index::BlobKind>> writes;
    std::size_t refused = 0;

    bool write(llvm::ArrayRef<Blob> puts, llvm::ArrayRef<index::BlobKey> removes) override {
        writes.emplace_back();
        for(auto& put: puts) {
            writes.back().push_back(put.kind);
        }
        bool committed = real->write(puts, removes);
        if(!committed) {
            refused += 1;
        }
        return committed;
    }
};

/// Open the cache store under `tmp` with a logged LMDB database of
/// `mapsize` bytes (0 = the default).
WriteLog& open_logged(TempDir& tmp, Project& project, std::size_t mapsize = 0) {
    auto store = CacheStore::open(tmp.path("cache"), 1);
    ZASSERT(store);
    project.store.emplace(std::move(*store));
    auto log = std::make_unique<WriteLog>(index::open_lmdb_database(*project.store, {}, mapsize));
    ZASSERT(log->real != nullptr);
    auto& logged = *log;
    project.database = std::move(log);
    return logged;
}

ZEST_SUITE(ProjectIndexSave) {

IndexerFixture fx;
kota::event_loop& loop = fx.loop;
Project& project = fx.project;
IndexPump& pump = fx.pump;

bool merge(llvm::StringRef data) {
    return fx.merge(data);
}

kota::task<> async_save() {
    return fx.async_save();
}

/// How many hits the persisted search index alone gives `text`.
std::size_t index_hits(llvm::StringRef text) {
    return project.index.search_index()
        .search(*index::SymbolQuery::parse(text), 10, {})
        .hits.size();
}

std::uint32_t stored_segments() {
    std::uint32_t segments = 0;
    project.database->for_each_key(index::BlobKind::SearchSegment,
                                   [&](llvm::StringRef) { segments += 1; });
    return segments;
}

ZEST_CASE(SaveCommitsDirtyShard) {
    TempDir tmp;
    tmp.touch("main.cpp", "int flip_value() { return 1; }\n");
    auto src = tmp.path("main.cpp");
    open_store(tmp, project);

    auto indexed = index_file(tmp, src);
    ZASSERT(!indexed.data.empty());
    merge(indexed.data);

    auto path_id = project.file_table.intern(Spelling::absolute(indexed.tu_path));
    ZASSERT(project.index.pending_shard_writes() == 1u);

    // Named body: a temporary lambda's captures die with the statement
    // while the coroutine frame still references them.
    auto save_body = [&]() -> kota::task<> {
        co_await async_save();
    };
    auto task = save_body();
    loop.schedule(task);
    loop.run();

    // Committed: the dirty state is drained and the shard still answers
    // identically.
    auto* it = project.index.shard(path_id);
    ZASSERT(it);
    ZASSERT(project.index.pending_shard_writes() == 0u);
    ZASSERT(project.index.last_save_shards() == 1u);
    ZASSERT(it->content_hash() == llvm::xxh3_64bits("int flip_value() { return 1; }\n"));
    ZASSERT(contributions_of(project.index, path_id).contains(path_id));
}

ZEST_CASE(SaveMigratesShardViews) {
    TempDir tmp;
    tmp.touch("main.cpp", "int migrate_value() { return 1; }\n");
    auto src = tmp.path("main.cpp");

    // The LMDB backend wrapped in a spy: save() must advance the read
    // snapshot exactly once, rebind the resident shard onto it, and
    // retire the old snapshot exactly once.
    struct SnapshotSpy final : ForwardingDatabase {
        using ForwardingDatabase::ForwardingDatabase;

        int advances = 0;
        int retires = 0;

        std::expected<std::uint64_t, std::string> advance_read_snapshot() override {
            advances += 1;
            return real->advance_read_snapshot();
        }

        void retire_old_snapshot() override {
            retires += 1;
            real->retire_old_snapshot();
        }
    };

    auto store = CacheStore::open(tmp.path("cache"), 1);
    ZASSERT(store);
    project.store.emplace(std::move(*store));
    auto spy = std::make_unique<SnapshotSpy>(index::open_lmdb_database(*project.store, {}));
    ZASSERT(spy->real != nullptr);
    auto* probe = spy.get();
    project.database = std::move(spy);

    auto indexed = index_file(tmp, src);
    ZASSERT(!indexed.data.empty());
    merge(indexed.data);
    auto path_id = project.file_table.intern(Spelling::absolute(indexed.tu_path));
    auto* before = project.index.shard(path_id);
    ZASSERT(before);
    auto variants_before = before->variants();
    const char* bytes_before = before->bytes().data();

    auto save_body = [&]() -> kota::task<> {
        co_await async_save();
    };
    auto task = save_body();
    loop.schedule(task);
    loop.run();

    // Rebound: the shard now serves the database's snapshot view — the
    // same bytes at a different address — with the verification state and
    // variant set carried over.
    ZASSERT(probe->advances == 1);
    ZASSERT(probe->retires == 1);
    auto* it = project.index.shard(path_id);
    ZASSERT(it);
    ZASSERT(it->loaded());
    ZASSERT(it->bytes().data() != bytes_before);
    ZASSERT(it->content_hash() == llvm::xxh3_64bits("int migrate_value() { return 1; }\n"));
    ZASSERT(it->variants() == variants_before);
}

ZEST_CASE(RefusedGrowthKeepsBatch) {
    TempDir tmp;
    tmp.touch("main.cpp", "int kept_value() { return 1; }\n");
    open_store(tmp, project);

    // A full map the address space cannot grow: the batch fails like any
    // failed write, nothing is lost, and a later save commits it.
    struct RefusedGrowth final : ForwardingDatabase {
        using ForwardingDatabase::ForwardingDatabase;

        bool full = true;

        bool write(llvm::ArrayRef<Blob> puts, llvm::ArrayRef<index::BlobKey> removes) override {
            return !full && real->write(puts, removes);
        }

        Growth grow() override {
            return full ? Growth::Refused : Growth::None;
        }
    };

    auto spy = std::make_unique<RefusedGrowth>(std::move(project.database));
    auto* probe = spy.get();
    project.database = std::move(spy);

    auto indexed = index_file(tmp, tmp.path("main.cpp"));
    ZASSERT(!indexed.data.empty());
    merge(indexed.data);
    auto file = project.file_table.intern(Spelling::absolute(indexed.tu_path));

    fx.save();
    ZASSERT(project.index.shard(file) != nullptr);
    ZASSERT(project.index.pending_shard_writes() == 1u);
    ZASSERT(has_unsaved_state(project));

    probe->full = false;
    fx.save();
    ZASSERT(project.index.pending_shard_writes() == 0u);
    ZASSERT(project.index.last_save_shards() == 1u);
    ZASSERT(!has_unsaved_state(project));
}

ZEST_CASE(MidSaveMergeKept) {
    TempDir tmp;
    tmp.touch("main.cpp", "int first_value() { return 1; }\n");
    auto src = tmp.path("main.cpp");
    open_store(tmp, project);

    auto indexed = index_file(tmp, src);
    ZASSERT(!indexed.data.empty());
    merge(indexed.data);
    auto path_id = project.file_table.intern(Spelling::absolute(indexed.tu_path));

    // Prepared before save() starts so the interleaved merge is purely an
    // in-memory event.
    tmp.touch("main.cpp", "int second_value() { return 2; }\n");
    auto fresh = index_file(tmp, src);
    ZASSERT(!fresh.data.empty());

    // The merge task runs when save() suspends at its write await: it
    // merges after the dirty snapshot was taken and cleared, exactly the
    // window re-dirtying exists for.
    auto save_body = [&]() -> kota::task<> {
        co_await async_save();
    };
    std::size_t mid_save_pending = 0;
    auto merge_body = [&]() -> kota::task<> {
        mid_save_pending = project.index.pending_shard_writes();
        merge(fresh.data);
        co_return;
    };
    auto save_task = save_body();
    auto merge_task = merge_body();
    loop.schedule(save_task);
    loop.schedule(merge_task);
    loop.run();

    // Sampled while save() awaited its commit: the pending-write gauge must
    // keep covering the in-flight batch, or a stats poll in that window
    // reads "finished" with last_save_shards still holding its reset.
    ZASSERT(mid_save_pending >= 1);

    // The save committed the pre-merge snapshot: the shard keeps the new
    // content and stays dirty so the next save commits it.
    auto* it = project.index.shard(path_id);
    ZASSERT(it);
    ZASSERT(project.index.pending_shard_writes() == 1u);
    ZASSERT(it->content_hash() == llvm::xxh3_64bits("int second_value() { return 2; }\n"));

    auto again_body = [&]() -> kota::task<> {
        co_await async_save();
    };
    auto task = again_body();
    loop.schedule(task);
    loop.run();

    it = project.index.shard(path_id);
    ZASSERT(project.index.pending_shard_writes() == 0u);
    ZASSERT(it->content_hash() == llvm::xxh3_64bits("int second_value() { return 2; }\n"));
}

ZEST_CASE(RetiredDuringMigration) {
    // A shard retired while the migration onto a fresh snapshot yields
    // between its batches is the retirement's: the migration leaves it.
    TempDir tmp;
    open_store(tmp, project);
    index::FileIndex file_index;
    file_index.relations[7].push_back({
        .kind = RelationKind::Definition,
        .range = {4, 5}
    });
    auto bytes = index::write_shard(file_index, {}, "int x = 1;\n");
    auto plant = [&](llvm::StringRef name) {
        auto file = project.file_table.intern(Spelling::absolute(name));
        IndexInternals::plant_shard(
            project.index,
            file,
            index::Shard::from_buffer(llvm::MemoryBuffer::getMemBufferCopy(as_chars(bytes))),
            /*written=*/true);
        return file;
    };
    std::vector<Fid> planted;
    for(std::uint32_t i = 0; i < 600; i += 1) {
        planted.push_back(plant(std::format("/proj/f{}.cpp", i)));
    }
    // The last the migration reaches: past its first batch.
    auto last = ranges::max(planted);
    fx.save();
    plant("/proj/next.cpp");

    auto save_task = fx.async_save();
    auto retire_body = [&]() -> kota::task<> {
        while(project.index.pending_shard_writes() != 0) {
            co_await kota::yield(loop);
        }
        IndexInternals::retire_shard(project.index, last);
    };
    auto retire_task = retire_body();
    loop.schedule(save_task);
    loop.schedule(retire_task);
    loop.run();
    ZEXPECT(!project.index.damaged());
    ZEXPECT(!project.index.shard(last));
    ZEXPECT(project.index.shard(planted.front()) != nullptr);
}

ZEST_CASE(RebuildKeepsPendingFindable) {
    TempDir tmp;
    tmp.touch("a.cpp", "int first_value() { return 1; }\n");
    tmp.touch("b.cpp", "int second_value() { return 2; }\n");
    open_store(tmp, project);
    auto first = index_file(tmp, tmp.path("a.cpp"));
    ZASSERT(!first.data.empty());
    merge(first.data);
    fx.save(index::SearchRebuild::Idle);
    ZASSERT(project.index.search_index().loaded());

    auto second = index_file(tmp, tmp.path("b.cpp"));
    ZASSERT(!second.data.empty());
    merge(second.data);
    llvm::SmallVector<index::SymbolHash> merged(project.index.search_pending().begin(),
                                                project.index.search_pending().end());
    ZASSERT(!merged.empty());

    auto indexed = [&](index::SymbolHash hash) {
        auto symbol = project.index.find_symbol(hash);
        auto query = index::SymbolQuery::parse(std::format(R"("{}")", symbol->name.str()));
        auto hits = project.index.search_index().search(*query, 10, {}).hits;
        return ranges::any_of(hits, [&](auto& hit) { return hit.hash == hash; });
    };

    // The probe runs while the save awaits the search build on the thread
    // pool: what was merged since the last build is in neither index yet,
    // so it must still be pending, where a search scans it.
    auto save_body = [&]() -> kota::task<> {
        co_await fx.async_save(index::SearchRebuild::Idle);
    };
    bool findable = true;
    auto probe_body = [&]() -> kota::task<> {
        for(auto hash: merged) {
            findable &= indexed(hash) || project.index.search_pending().contains(hash);
        }
        co_return;
    };
    auto save_task = save_body();
    auto probe_task = probe_body();
    loop.schedule(save_task);
    loop.schedule(probe_task);
    loop.run();

    ZASSERT(findable);
    for(auto hash: merged) {
        ZASSERT(indexed(hash));
        ZASSERT(!project.index.search_pending().contains(hash));
    }
}

ZEST_CASE(RebuildThroughUnnamedScopes) {
    TempDir tmp;
    tmp.touch("main.cpp",
              "namespace ns { enum { kSize = 4 }; }\nint main() { return ns::kSize; }\n");
    open_store(tmp, project);
    auto indexed = index_file(tmp, tmp.path("main.cpp"));
    ZASSERT(!indexed.data.empty());
    merge(indexed.data);
    fx.save(index::SearchRebuild::Idle);
    auto& search = project.index.search_index();
    ZASSERT(search.loaded());

    auto hits = [&](llvm::StringRef text) {
        return search.search(*index::SymbolQuery::parse(text), 10, {}).hits.size();
    };
    ZEXPECT(hits("ns::kSize") == 1u);
    ZEXPECT(hits("::kSize") == 0u);
}

ZEST_CASE(RebuildKeepsMidBuildChanges) {
    TempDir tmp;
    tmp.touch("a.cpp", "int first_value() { return 1; }\n");
    tmp.touch("b.cpp", "int second_value() { return 2; }\n");
    tmp.touch("c.cpp", "int second_value() { return 3; }\n");
    open_store(tmp, project);
    auto first = index_file(tmp, tmp.path("a.cpp"));
    ZASSERT(!first.data.empty());
    merge(first.data);
    fx.save(index::SearchRebuild::Idle);

    auto second = index_file(tmp, tmp.path("b.cpp"));
    ZASSERT(!second.data.empty());
    merge(second.data);
    llvm::SmallVector<index::SymbolHash> merged(project.index.search_pending().begin(),
                                                project.index.search_pending().end());
    ZASSERT(!merged.empty());
    auto moved = index_file(tmp, tmp.path("c.cpp"));
    ZASSERT(!moved.data.empty());

    // The definition moves to c.cpp while the build reads the table from
    // before: the built entry describes the old file, so the symbol stays
    // pending, where a search reads the table.
    auto save_body = [&]() -> kota::task<> {
        co_await fx.async_save(index::SearchRebuild::Idle);
    };
    auto merge_body = [&]() -> kota::task<> {
        merge(moved.data);
        co_return;
    };
    auto save_task = save_body();
    auto merge_task = merge_body();
    loop.schedule(save_task);
    loop.schedule(merge_task);
    loop.run();

    for(auto hash: merged) {
        ZASSERT(project.index.search_pending().contains(hash));
    }
}

ZEST_CASE(SavesAddRuns) {
    // Every save adds what changed as a run of its own; a save that may
    // fold the newest runs, and an idle one the whole index, once they
    // outgrow the runs they fold into.
    TempDir tmp;
    tmp.touch("a.cpp", "int first_value() { return 1; }\nstatic int first_local = 1;\n");
    tmp.touch("b.cpp", "int second_value() { return 2; }\nstatic int second_local = 2;\n");
    tmp.touch("c.cpp", "int third_value() { return 3; }\n");
    open_store(tmp, project);
    auto& search = project.index.search_index();
    ZASSERT(merge(index_file(tmp, tmp.path("a.cpp")).data));
    fx.save(index::SearchRebuild::Idle);
    ZASSERT(search.run_count() == 1u);

    ZASSERT(merge(index_file(tmp, tmp.path("b.cpp")).data));
    fx.save(index::SearchRebuild::Never);
    ZEXPECT(search.run_count() == 2u);
    ZEXPECT(project.index.search_pending().empty());
    ZEXPECT(index_hits("second_value") == 1u);

    // The run of b.cpp folds into the one of c.cpp, which reads its
    // symbols and names of internal linkage again.
    ZASSERT(merge(index_file(tmp, tmp.path("c.cpp")).data));
    fx.save(index::SearchRebuild::Behind);
    ZEXPECT(search.run_count() == 2u);
    ZEXPECT(stored_segments() == 2u);
    for(auto name: {"second_value", "second_local", "third_value"}) {
        ZEXPECT(index_hits(name) == 1u);
    }

    ZEXPECT(project.index.search_update_due(index::SearchRebuild::Idle));
    fx.save(index::SearchRebuild::Idle);
    ZEXPECT(search.run_count() == 1u);
    ZEXPECT(stored_segments() == 1u);
    for(auto name: {"first_value", "first_local", "second_value", "second_local", "third_value"}) {
        ZEXPECT(index_hits(name) == 1u);
    }
}

ZEST_CASE(FoldsKeepRunsFew) {
    // Saves of like-sized changes fold as they go: the runs stay few, and
    // every symbol stays found.
    TempDir tmp;
    tmp.touch("base.cpp", "int base_value() { return 0; }\n");
    open_store(tmp, project);
    ZASSERT(merge(index_file(tmp, tmp.path("base.cpp")).data));
    fx.save(index::SearchRebuild::Idle);
    for(std::uint32_t i = 0; i < 8; i += 1) {
        auto file = std::format("unit{}.cpp", i);
        tmp.touch(file, std::format("int unit_value_{}() {{ return {}; }}\n", i, i));
        ZASSERT(merge(index_file(tmp, tmp.path(file)).data));
        fx.save(index::SearchRebuild::Behind);
    }
    ZEXPECT(project.index.search_index().run_count() <= 3u);
    for(std::uint32_t i = 0; i < 8; i += 1) {
        ZEXPECT(index_hits(std::format(R"("unit_value_{}")", i)) == 1u);
    }
}

ZEST_CASE(NewestRunHidesOlder) {
    // Checkpoints pile up runs unfolded: the newest one says what holds,
    // and a fold of them all keeps that.
    TempDir tmp;
    tmp.touch("a.cpp", "int first_value() { return 1; }\n");
    tmp.touch("b.cpp", "int second_value() { return 2; }\nstatic int old_name = 1;\n");
    tmp.touch("c.cpp", "int third_value() { return 3; }\n");
    open_store(tmp, project);
    auto& search = project.index.search_index();
    ZASSERT(merge(index_file(tmp, tmp.path("a.cpp")).data));
    fx.save(index::SearchRebuild::Idle);
    ZASSERT(merge(index_file(tmp, tmp.path("b.cpp")).data));
    fx.save(index::SearchRebuild::Never);
    tmp.touch("b.cpp", "int second_value() { return 2; }\nstatic int new_name = 1;\n");
    ZASSERT(merge(index_file(tmp, tmp.path("b.cpp")).data));
    fx.save(index::SearchRebuild::Never);
    ZASSERT(search.run_count() == 3u);
    ZEXPECT(index_hits("old_name") == 0u);
    ZEXPECT(index_hits("new_name") == 1u);

    ZASSERT(merge(index_file(tmp, tmp.path("c.cpp")).data));
    fx.save(index::SearchRebuild::Behind);
    ZEXPECT(search.run_count() == 2u);
    ZEXPECT(stored_segments() == 2u);
    ZEXPECT(index_hits("old_name") == 0u);
    for(auto name: {"first_value", "second_value", "new_name", "third_value"}) {
        ZEXPECT(index_hits(name) == 1u);
    }
}

ZEST_CASE(RunResolvesScopes) {
    // A run of a few symbols carries the scopes above them, as a whole
    // build does, without offering them twice.
    TempDir tmp;
    tmp.touch("a.cpp", "namespace ns { int first_value() { return 1; } }\n");
    tmp.touch("b.cpp", "namespace ns { int second_value() { return 2; } }\n");
    open_store(tmp, project);
    ZASSERT(merge(index_file(tmp, tmp.path("a.cpp")).data));
    fx.save(index::SearchRebuild::Idle);
    ZASSERT(merge(index_file(tmp, tmp.path("b.cpp")).data));
    fx.save(index::SearchRebuild::Never);
    ZASSERT(project.index.search_index().run_count() == 2u);
    ZEXPECT(index_hits("ns::second_value") == 1u);
    ZEXPECT(index_hits("::ns::second_value") == 1u);
    ZEXPECT(index_hits("::second_value") == 0u);
    ZEXPECT(index_hits(R"("ns")") == 1u);
    ZEXPECT(index_hits("ns::*") == 2u);
}

ZEST_CASE(CountBucketsRequalify) {
    // A symbol whose reference count crosses a power of two is read again
    // by the next run, its quality moved; one that only grows is not.
    TempDir tmp;
    tmp.touch("shared.h", "#pragma once\ninline int shared_fn() { return 1; }\n");
    for(auto unit: {"a", "b", "c", "d", "e"}) {
        tmp.touch(std::format("{}.cpp", unit),
                  std::format("#include \"shared.h\"\nint {}() {{ return shared_fn(); }}\n", unit));
    }
    open_store(tmp, project);
    ZASSERT(merge(index_file(tmp, tmp.path("a.cpp")).data));
    fx.save(index::SearchRebuild::Idle);
    auto hits = project.index.search_index()
                    .search(*index::SymbolQuery::parse(R"("shared_fn")"), 10, {})
                    .hits;
    ZASSERT(hits.size() == 1u);
    auto shared = hits.front().hash;
    std::uint32_t requalified = 0;
    std::uint32_t kept = 0;
    for(auto unit: {"b", "c", "d", "e"}) {
        auto before = project.index.reference_count(shared);
        ZASSERT(merge(index_file(tmp, tmp.path(std::format("{}.cpp", unit))).data));
        auto after = project.index.reference_count(shared);
        ZASSERT(after == before + 1);
        bool pending = project.index.search_pending().contains(shared);
        ZEXPECT(pending == (std::bit_width(before) != std::bit_width(after)));
        if(pending) {
            requalified += 1;
        } else {
            kept += 1;
        }
        fx.save(index::SearchRebuild::Never);
    }
    ZEXPECT(requalified > 0u);
    ZEXPECT(kept > 0u);
}

ZEST_CASE(ReindexHidesOldNames) {
    // A file indexed again replaces its names of internal linkage, and a
    // symbol its old ones in the older runs.
    TempDir tmp;
    tmp.touch("a.cpp", "static int old_local = 1;\nint use() { return old_local; }\n");
    open_store(tmp, project);
    ZASSERT(merge(index_file(tmp, tmp.path("a.cpp")).data));
    fx.save(index::SearchRebuild::Idle);
    ZASSERT(index_hits("old_local") == 1u);

    tmp.touch("a.cpp", "static int new_local = 1;\nint use() { return new_local; }\n");
    ZASSERT(merge(index_file(tmp, tmp.path("a.cpp")).data));
    fx.save(index::SearchRebuild::Never);
    ZASSERT(project.index.search_index().run_count() == 2u);
    ZEXPECT(index_hits("old_local") == 0u);
    ZEXPECT(index_hits("new_local") == 1u);
    ZEXPECT(index_hits("use") == 1u);
}

ZEST_CASE(FailedSaveKeepsSearch) {
    // A write failing after the search index was rebuilt: the build stays
    // for the retry, and searches keep finding what it holds.
    struct FlakyStorage final : ForwardingDatabase {
        using ForwardingDatabase::ForwardingDatabase;

        bool fail_write = false;
        bool fail_advance = false;

        bool write(llvm::ArrayRef<Blob> puts, llvm::ArrayRef<index::BlobKey> removes) override {
            return !fail_write && real->write(puts, removes);
        }

        std::expected<std::uint64_t, std::string> advance_read_snapshot() override {
            if(fail_advance) {
                return std::unexpected(std::string("every reader slot taken"));
            }
            return real->advance_read_snapshot();
        }
    };

    TempDir tmp;
    tmp.touch("a.cpp", "int first_value() { return 1; }\n");
    tmp.touch("b.cpp", "int second_value() { return 2; }\n");
    tmp.touch("c.cpp", "int third_value() { return 3; }\n");
    open_store(tmp, project);
    auto flaky = std::make_unique<FlakyStorage>(std::move(project.database));
    auto* probe = flaky.get();
    project.database = std::move(flaky);
    auto hits = [&](llvm::StringRef name) {
        return project.index.search_index()
            .search(*index::SymbolQuery::parse(name), 10, {})
            .hits.size();
    };

    auto first = index_file(tmp, tmp.path("a.cpp"));
    ZASSERT(!first.data.empty());
    merge(first.data);
    fx.save(index::SearchRebuild::Idle);
    ZASSERT(hits("first_value") == 1u);

    auto second = index_file(tmp, tmp.path("b.cpp"));
    ZASSERT(!second.data.empty());
    merge(second.data);
    probe->fail_write = true;
    fx.save(index::SearchRebuild::Idle);
    ZEXPECT(hits("second_value") == 1u);
    ZEXPECT(has_unsaved_state(project));

    // The retry commits the build but cannot move onto a fresh snapshot;
    // the next batch, with the run of what c.cpp added, fails, and the idle
    // save after it builds the index again over that run, which no batch
    // committed: the database keeps only the segment the header lists.
    probe->fail_write = false;
    probe->fail_advance = true;
    fx.save(index::SearchRebuild::Idle);
    probe->fail_advance = false;
    auto third = index_file(tmp, tmp.path("c.cpp"));
    ZASSERT(!third.data.empty());
    merge(third.data);
    probe->fail_write = true;
    fx.save();
    probe->fail_write = false;
    fx.save(index::SearchRebuild::Idle);
    ZEXPECT(!has_unsaved_state(project));
    ZEXPECT(hits("second_value") == 1u);
    ZEXPECT(hits("third_value") == 1u);
    ZEXPECT(stored_segments() == 1u);
}

ZEST_CASE(SaveCompactsAndRetires) {
    TempDir tmp;
    tmp.touch("shared.h",
              "#pragma once\n#ifdef MODE\nint mode_fn();\n#endif\n"
              "inline int shared_fn() { return 1; }\n");
    tmp.touch("a.cpp", "#include \"shared.h\"\nint a() { return shared_fn(); }\n");
    tmp.touch("b.cpp", "#include \"shared.h\"\nint b() { return shared_fn(); }\n");
    open_store(tmp, project);

    auto a = index_file(tmp, tmp.path("a.cpp"));
    auto b = index_file(tmp, tmp.path("b.cpp"), {"-DMODE"});
    ZASSERT(!a.data.empty());
    ZASSERT(!b.data.empty());
    merge(a.data);
    merge(b.data);
    auto header_id = project.file_table.intern(Spelling::absolute(tmp.path("shared.h")));
    ZASSERT(project.index.shard(header_id)->variants().size() == std::size_t(2));

    auto save = [&] {
        auto body = [&]() -> kota::task<> {
            co_await async_save();
        };
        auto task = body();
        loop.schedule(task);
        loop.run();
    };
    save();

    // b stops including the header: its variant dies, and the next save
    // erases the dead variant for real.
    tmp.touch("b.cpp", "int b() { return 2; }\n");
    auto b2 = index_file(tmp, tmp.path("b.cpp"));
    ZASSERT(!b2.data.empty());
    merge(b2.data);
    ZASSERT(project.index.shard(header_id)->has_dead_variants());
    save();
    ZASSERT(project.index.shard(header_id)->variants().size() == std::size_t(1));

    // a drops it too: no contribution is left, so the shard retires from
    // memory and from storage — with no owner left to re-enqueue.
    tmp.touch("a.cpp", "int a() { return 3; }\n");
    auto a2 = index_file(tmp, tmp.path("a.cpp"));
    ZASSERT(!a2.data.empty());
    merge(a2.data);
    save();
    ZASSERT(!project.index.shard(header_id));
    ZASSERT(!pump.pending_reason(project.file_table.intern(Spelling::absolute(a2.tu_path)))
                 .has_value());
    ZASSERT(!project.database->contains(index::BlobKind::Shard,
                                        IndexInternals::file_key(project.index, header_id)));
}

ZEST_CASE(SaveRetiresPinnedShard) {
    TempDir tmp;
    tmp.touch("pinned.h",
              "#pragma once\n#ifdef MODE\nint pin_mode();\n#endif\n"
              "inline int pin_fn() { return 1; }\n");
    tmp.touch("pa.cpp", "#include \"pinned.h\"\nint pa() { return pin_fn(); }\n");
    tmp.touch("pb.cpp", "#include \"pinned.h\"\nint pb() { return pin_fn(); }\n");
    open_store(tmp, project);

    auto a = index_file(tmp, tmp.path("pa.cpp"));
    auto b = index_file(tmp, tmp.path("pb.cpp"), {"-DMODE"});
    ZASSERT(!a.data.empty());
    ZASSERT(!b.data.empty());
    merge(a.data);
    merge(b.data);
    auto header_id = project.file_table.intern(Spelling::absolute(tmp.path("pinned.h")));
    ZASSERT(project.index.shard(header_id)->variants().size() == std::size_t(2));

    // The header moves to a new content generation and only pa catches up:
    // the blob starts over with pa's variant, while pb's manifest still
    // pins a hash the blob no longer stores.
    tmp.touch("pinned.h",
              "#pragma once\n#ifdef MODE\nint pin_mode();\n#endif\n"
              "inline int pin_fn() { return 2; }\n");
    auto a2 = index_file(tmp, tmp.path("pa.cpp"));
    ZASSERT(!a2.data.empty());
    merge(a2.data);
    ZASSERT(project.index.shard(header_id)->variants().size() == std::size_t(1));

    // The rebuild re-enqueued pb; its pass then runs and fails, consuming
    // the slot — the state the retirement below must repair on its own.
    pump.clear_pending(project.file_table.intern(Spelling::absolute(b.tu_path)));

    // pa's index drops before pb reindexes: every stored variant is dead,
    // but pb's pinned hash keeps the live set nonempty. The save must
    // retire the shard rather than compact to an empty variant set.
    fx.drop_index(project.file_table.intern(Spelling::absolute(a2.tu_path)));
    auto body = [&]() -> kota::task<> {
        co_await async_save();
    };
    auto task = body();
    loop.schedule(task);
    loop.run();

    ZASSERT(!project.index.shard(header_id));
    ZASSERT(!project.database->contains(index::BlobKind::Shard,
                                        IndexInternals::file_key(project.index, header_id)));

    // pb's manifest survives, still pinning variants the retirement made
    // unservable; nothing else in this process would rebuild them (a
    // reverted header even reads fresh by hash), so the retirement must
    // re-enqueue pb itself.
    ZASSERT(pump.pending_reason(project.file_table.intern(Spelling::absolute(b.tu_path))) ==
            ReindexReason::ContentChanged);
}

ZEST_CASE(FailedWriteNotCounted) {
    TempDir tmp;
    tmp.touch("main.cpp", "int uncommitted() { return 1; }\n");
    auto src = tmp.path("main.cpp");

    // A storage whose commits never succeed (disk full, permissions): the
    // gauge must report what was durably committed, not what the save
    // attempted.
    struct FailingStorage final : ForwardingDatabase {
        using ForwardingDatabase::ForwardingDatabase;

        bool fail = true;

        bool write(llvm::ArrayRef<Blob> puts, llvm::ArrayRef<index::BlobKey> removes) override {
            return !fail && real->write(puts, removes);
        }
    };

    open_store(tmp, project);
    auto failing = std::make_unique<FailingStorage>(std::move(project.database));
    auto* probe = failing.get();
    project.database = std::move(failing);

    auto indexed = index_file(tmp, src);
    ZASSERT(!indexed.data.empty());
    merge(indexed.data);
    ZASSERT(project.index.pending_shard_writes() == 1u);

    auto save = [&] {
        auto body = [&]() -> kota::task<> {
            co_await async_save();
        };
        auto task = body();
        loop.schedule(task);
        loop.run();
    };
    save();
    ZASSERT(project.index.last_save_shards() == 0u);
    // The failed batch is re-dirtied rather than discarded, so a later
    // save has it to retry and the cache converges once the storage
    // recovers.
    ZASSERT(project.index.pending_shard_writes() == 1u);

    probe->fail = false;
    save();
    ZASSERT(project.index.last_save_shards() == 1u);
    ZASSERT(project.index.pending_shard_writes() == 0u);

    // Every record the failed batch carried reached the database with the
    // retry: another reader finds the symbol in its file, and the unit.
    clice::FileTable files;
    index::ProjectIndex reader{files};
    ZASSERT(reader.open(*project.database) == index::LoadOutcome::Loaded);
    auto unit = files.intern(Spelling::absolute(indexed.tu_path));
    ZEXPECT(reader.indexed(unit));
    ZEXPECT(reader.manifest(unit).has_value());
    Fid declared;
    reader.for_each_symbol([&](index::SymbolHash, const index::SymbolView& view, std::uint32_t) {
        if(view.name == "uncommitted") {
            declared = Fid{view.file};
        }
        return !declared.valid();
    });
    ZEXPECT(declared == unit);
}

ZEST_CASE(WriteCorruptionRebuildsDatabase) {
    TempDir tmp;
    tmp.touch("clean.cpp", "int clean_value() { return 1; }\n");
    tmp.touch("dirty.cpp", "int dirty_value() { return 2; }\n");
    open_store(tmp, project);
    for(auto name: {"clean.cpp", "dirty.cpp"}) {
        auto command = std::format("clang++ -c {}", name);
        project.cdb.add_command(tmp.root, tmp.path(name), llvm::StringRef(command));
    }

    // Corruption showing up at write time (a damaged page only the write's
    // tree descent reaches): the save condemns the environment and the
    // project starts over on a fresh one, every unit indexed again.
    struct CorruptOnWrite final : ForwardingDatabase {
        using ForwardingDatabase::ForwardingDatabase;

        bool* condemned;
        bool fail = false;
        bool poisoned = false;

        bool write(llvm::ArrayRef<Blob> puts, llvm::ArrayRef<index::BlobKey> removes) override {
            if(fail) {
                poisoned = true;
                return false;
            }
            return real->write(puts, removes);
        }

        bool corrupted() const override {
            return poisoned;
        }

        void condemn() override {
            *condemned = true;
            real->condemn();
        }
    };

    bool condemned = false;
    auto spy = std::make_unique<CorruptOnWrite>(std::move(project.database));
    spy->condemned = &condemned;
    auto* probe = spy.get();
    project.database = std::move(spy);

    auto indexed_clean = index_file(tmp, tmp.path("clean.cpp"));
    auto indexed_dirty = index_file(tmp, tmp.path("dirty.cpp"));
    ZASSERT(!indexed_clean.data.empty());
    ZASSERT(!indexed_dirty.data.empty());

    merge(indexed_clean.data);
    fx.save();
    merge(indexed_dirty.data);
    probe->fail = true;
    fx.save();

    ZASSERT(condemned);
    ZASSERT(project.database != nullptr);
    auto clean_id = project.file_table.intern(Spelling::absolute(indexed_clean.tu_path));
    auto dirty_id = project.file_table.intern(Spelling::absolute(indexed_dirty.tu_path));
    ZASSERT(!project.index.shard(clean_id));
    ZASSERT(!project.index.shard(dirty_id));
    ZASSERT(pump.pending_reason(clean_id) == ReindexReason::ContentChanged);
    ZASSERT(pump.pending_reason(dirty_id) == ReindexReason::ContentChanged);
    ZASSERT(project.index.last_save_shards() == 0u);

    // The next save records the command snapshot in the fresh database.
    fx.save();
    ZASSERT(!has_unsaved_state(project));
}

ZEST_CASE(MigrationCorruptionRebuildsDatabase) {
    TempDir tmp;
    tmp.touch("main.cpp", "int migrate_value() { return 1; }\n");
    open_store(tmp, project);
    project.cdb.add_command(tmp.root, tmp.path("main.cpp"), llvm::StringRef("clang++ -c main.cpp"));

    // Corruption showing up first at migration time (a damaged page only the
    // re-read from the advanced snapshot reaches, after the write-time
    // check passed): the same recovery as at write time.
    struct CorruptOnRead final : ForwardingDatabase {
        using ForwardingDatabase::ForwardingDatabase;

        bool* condemned;
        bool written = false;
        bool poisoned = false;

        index::ReadBlob read(index::BlobKind kind, llvm::StringRef key) override {
            if(written) {
                poisoned = true;
                return {};
            }
            return real->read(kind, key);
        }

        bool write(llvm::ArrayRef<Blob> puts, llvm::ArrayRef<index::BlobKey> removes) override {
            written = real->write(puts, removes);
            return written;
        }

        bool corrupted() const override {
            return poisoned;
        }

        void condemn() override {
            *condemned = true;
            real->condemn();
        }
    };

    bool condemned = false;
    auto spy = std::make_unique<CorruptOnRead>(std::move(project.database));
    spy->condemned = &condemned;
    project.database = std::move(spy);

    auto indexed = index_file(tmp, tmp.path("main.cpp"));
    ZASSERT(!indexed.data.empty());
    merge(indexed.data);
    fx.save();

    auto path_id = project.file_table.intern(Spelling::absolute(indexed.tu_path));
    ZASSERT(condemned);
    ZASSERT(project.database != nullptr);
    ZASSERT(!project.index.shard(path_id));
    ZASSERT(pump.pending_reason(path_id) == ReindexReason::ContentChanged);
    ZASSERT(project.index.last_save_shards() == 0u);

    fx.save();
    ZASSERT(!has_unsaved_state(project));
}

ZEST_CASE(GrownMapRetriesNow) {
    TempDir tmp;
    std::string header = "#pragma once\n";
    for(int i = 0; i < 2000; i += 1) {
        header += std::format("inline int function_{0}(int x) {{ return x + {0}; }}\n", i);
    }
    tmp.touch("big.h", header);
    tmp.touch("main.cpp", "#include \"big.h\"\nint use() { return function_1(0); }\n");

    IndexerFixture f;
    // Far smaller than the batch: the first write fails whole and the map
    // grows under it.
    auto& log = open_logged(tmp, f.project, 64 * 1024);
    auto indexed = index_file(tmp, tmp.path("main.cpp"));
    ZASSERT(!indexed.data.empty());
    ZASSERT(f.merge(indexed.data));

    auto task = f.pump.persist();
    f.loop.schedule(task);
    f.loop.run();

    ZASSERT(log.refused == 1u);
    ZASSERT(log.writes.size() == 2u);
    ZASSERT(!has_unsaved_state(f.project));
}

ZEST_CASE(ShutdownSkipsSearchRebuild) {
    TempDir tmp;
    tmp.touch("main.cpp", "int use() { return 1; }\n");

    kota::event_loop loop;
    FileTable files;
    Project project{files};
    WorkerPool pool{loop};
    CommandResolver commands{project};
    SchedulingStack stack(loop, project, commands, pool);
    auto& log = open_logged(tmp, project);
    auto indexed = index_file(tmp, tmp.path("main.cpp"));
    ZASSERT(!indexed.data.empty());
    auto report = project.index.merge(indexed.data, {});
    ZASSERT(report);
    stack.pump.apply_report(*report);

    auto task = stack.shutdown();
    loop.schedule(task);
    loop.run();

    // The shards commit without the search index a cold round never built.
    ZASSERT(log.writes.size() == 1u);
    ZASSERT(ranges::contains(log.writes[0], index::BlobKind::TUManifest));
    ZASSERT(!ranges::contains(log.writes[0], index::BlobKind::SearchSegment));
    ZASSERT(!project.index.search_index().loaded());
}

ZEST_CASE(InstantiationsPersist) {
    TempDir tmp;
    tmp.touch("box.h", R"(#pragma once
template <typename T>
struct Box {
    T value;
    void show() { value.print(); }
};
)");
    tmp.touch("a.cpp", R"(#include "box.h"
struct A { void print(); };
void a() { Box<A>{}.show(); }
)");
    std::vector<std::uint64_t> stored;
    std::uint32_t records = 0;
    auto count = [&](const index::InstantiationShard& shard) {
        std::uint32_t count = 0;
        shard.for_each_record([&](const index::InstantiationRecord&) {
            count += 1;
            return true;
        });
        return count;
    };
    {
        IndexerFixture f;
        open_store(tmp, f.project);
        auto indexed = index_file(tmp, tmp.path("a.cpp"));
        ZASSERT(f.merge(indexed.data));
        auto box_id = f.project.file_table.intern(Spelling::absolute(tmp.path("box.h")));
        auto* instantiations = f.project.index.instantiation_shard(box_id);
        stored.assign(instantiations->resolutions().begin(), instantiations->resolutions().end());
        records = count(*instantiations);
        f.save();
    }

    IndexerFixture f;
    open_store(tmp, f.project);
    f.load();
    auto box_id = f.project.file_table.intern(Spelling::absolute(tmp.path("box.h")));
    auto* instantiations = f.project.index.instantiation_shard(box_id);
    ZASSERT(instantiations);
    ZEXPECT(ranges::equal(instantiations->resolutions(), stored));
    ZEXPECT(f.project.index.live_uses(box_id).size() == 1u);
    ZEXPECT(count(*instantiations) == records);
    ZEXPECT(
        !f.pump.pending_reason(f.project.file_table.intern(Spelling::absolute(tmp.path("a.cpp")))));
}

ZEST_CASE(DamagedInstantiationsRebuildAll) {
    // An instantiation blob failing its checks damages the database like
    // any record: the next save discards it and every unit is indexed again.
    TempDir tmp;
    tmp.touch("box.h", R"(#pragma once
template <typename T>
struct Box {
    T value;
    void show() { value.print(); }
};
)");
    tmp.touch("a.cpp", R"(#include "box.h"
struct A { void print(); };
void a() { Box<A>{}.show(); }
)");
    auto command = [&](IndexerFixture& f) {
        f.project.cdb.add_command(tmp.root, tmp.path("a.cpp"), llvm::StringRef("clang++ -c a.cpp"));
    };
    std::string box_key;
    {
        IndexerFixture f;
        open_store(tmp, f.project);
        command(f);
        auto indexed = index_file(tmp, tmp.path("a.cpp"));
        ZASSERT(f.merge(indexed.data));
        f.save();
        box_key = IndexInternals::file_key(
            f.project.index,
            f.project.file_table.intern(Spelling::absolute(tmp.path("box.h"))));
    }
    inject_blob(tmp, index::BlobKind::InstantiationShard, box_key, "corrupted beyond verification");

    IndexerFixture f;
    open_store(tmp, f.project);
    command(f);
    f.load();
    auto box_id = f.project.file_table.intern(Spelling::absolute(tmp.path("box.h")));
    ZEXPECT(!f.project.index.instantiation_shard(box_id));
    ZEXPECT(f.project.index.damaged());
    f.save();
    ZEXPECT(!f.project.index.damaged());
    ZEXPECT(!f.project.database->contains(index::BlobKind::InstantiationShard, box_key));
    ZEXPECT(
        f.pump.pending_reason(f.project.file_table.intern(Spelling::absolute(tmp.path("a.cpp")))));
}

};  // ZEST_SUITE(ProjectIndexSave)

}  // namespace
}  // namespace clice::testing

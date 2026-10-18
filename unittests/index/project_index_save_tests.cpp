module;

#include "modules/prelude.h"

module clice;

import :index.database;
import :index.project_index;
import :index.symbol;
import :index.symbol_query;
import :project.command_resolver;
import :project.project;
import :project.save;
import :sched.index.pending_table;
import :sched.index.pump;
import :sched.stack;
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
struct WriteLog final : index::BlobDatabase {
    std::unique_ptr<index::BlobDatabase> real;
    std::vector<std::vector<index::BlobKind>> writes;
    std::size_t refused = 0;

    index::ReadBlob read(index::BlobKind kind, llvm::StringRef key) override {
        return real->read(kind, key);
    }

    bool contains(index::BlobKind kind, llvm::StringRef key) override {
        return real->contains(kind, key);
    }

    llvm::SmallVector<std::uint32_t> write(llvm::ArrayRef<Blob> puts,
                                           llvm::ArrayRef<index::BlobKey> removes) override {
        writes.emplace_back();
        for(auto& put: puts) {
            writes.back().push_back(put.kind);
        }
        auto failed = real->write(puts, removes);
        if(!failed.empty()) {
            refused += 1;
        }
        return failed;
    }

    void for_each_key(index::BlobKind kind, llvm::function_ref<void(llvm::StringRef)> fn) override {
        real->for_each_key(kind, fn);
    }

    std::expected<std::uint64_t, std::string> advance_read_snapshot() override {
        return real->advance_read_snapshot();
    }

    void retire_old_snapshot() override {
        real->retire_old_snapshot();
    }

    std::expected<bool, std::string> grow() override {
        return real->grow();
    }

    bool corrupted() const override {
        return real->corrupted();
    }
};

/// Open the cache store under `tmp` with a logged LMDB database of
/// `mapsize` bytes (0 = the default).
WriteLog& open_logged(TempDir& tmp, Project& project, std::size_t mapsize = 0) {
    auto store = CacheStore::open(tmp.path("cache"), 1);
    ZASSERT(store);
    project.store.emplace(std::move(*store));
    auto log = std::make_unique<WriteLog>();
    log->real = index::open_lmdb_database(*project.store, "", mapsize);
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
    struct SnapshotSpy final : index::BlobDatabase {
        std::unique_ptr<index::BlobDatabase> real;
        int advances = 0;
        int retires = 0;

        index::ReadBlob read(index::BlobKind kind, llvm::StringRef key) override {
            return real->read(kind, key);
        }

        bool contains(index::BlobKind kind, llvm::StringRef key) override {
            return real->contains(kind, key);
        }

        llvm::SmallVector<std::uint32_t> write(llvm::ArrayRef<Blob> puts,
                                               llvm::ArrayRef<index::BlobKey> removes) override {
            return real->write(puts, removes);
        }

        void for_each_key(index::BlobKind kind,
                          llvm::function_ref<void(llvm::StringRef)> fn) override {
            real->for_each_key(kind, fn);
        }

        std::expected<std::uint64_t, std::string> advance_read_snapshot() override {
            advances += 1;
            return real->advance_read_snapshot();
        }

        void retire_old_snapshot() override {
            retires += 1;
            real->retire_old_snapshot();
        }

        std::expected<bool, std::string> grow() override {
            return real->grow();
        }
    };

    auto store = CacheStore::open(tmp.path("cache"), 1);
    ZASSERT(store);
    project.store.emplace(std::move(*store));
    auto spy = std::make_unique<SnapshotSpy>();
    spy->real = index::open_lmdb_database(*project.store, "");
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

ZEST_CASE(GrowFailureShedsShards) {
    TempDir tmp;
    tmp.touch("clean.cpp", "int clean_value() { return 1; }\n");
    tmp.touch("dirty.cpp", "int dirty_value() { return 2; }\n");
    open_store(tmp, project);

    // grow() failing is backend-independent shed territory: every clean
    // (non-dirty, possibly borrowed) shard must go with its owner requeued
    // — the manifests still read fresh, so nothing else would rebuild the
    // dropped shards — while dirty ones are owned by construction and stay.
    // The spy also fails dirty.cpp's put so it is re-dirtied by the time
    // the migration runs.
    struct FailingGrow final : index::BlobDatabase {
        std::unique_ptr<index::BlobDatabase> real;
        std::string fail_key;

        index::ReadBlob read(index::BlobKind kind, llvm::StringRef key) override {
            return real->read(kind, key);
        }

        bool contains(index::BlobKind kind, llvm::StringRef key) override {
            return real->contains(kind, key);
        }

        llvm::SmallVector<std::uint32_t> write(llvm::ArrayRef<Blob> puts,
                                               llvm::ArrayRef<index::BlobKey> removes) override {
            auto failed = real->write(puts, removes);
            for(std::size_t i = 0; i < puts.size(); i += 1) {
                if(puts[i].key == fail_key && !llvm::is_contained(failed, i)) {
                    failed.push_back(i);
                }
            }
            return failed;
        }

        void for_each_key(index::BlobKind kind,
                          llvm::function_ref<void(llvm::StringRef)> fn) override {
            real->for_each_key(kind, fn);
        }

        std::expected<std::uint64_t, std::string> advance_read_snapshot() override {
            return real->advance_read_snapshot();
        }

        void retire_old_snapshot() override {
            real->retire_old_snapshot();
        }

        std::expected<bool, std::string> grow() override {
            return std::unexpected(std::string("address space exhausted"));
        }
    };

    auto indexed_clean = index_file(tmp, tmp.path("clean.cpp"));
    auto indexed_dirty = index_file(tmp, tmp.path("dirty.cpp"));
    ZASSERT(!indexed_clean.data.empty());
    ZASSERT(!indexed_dirty.data.empty());

    auto spy = std::make_unique<FailingGrow>();
    spy->real = std::move(project.database);
    // Through the pool: the indexer keys blobs by the pool-canonical path,
    // which need not equal the raw temp path byte-for-byte (Windows 8.3
    // names).
    spy->fail_key = index::blob_key(project.file_table.resolve(
        project.file_table.intern(Spelling::absolute(indexed_dirty.tu_path))));
    project.database = std::move(spy);

    merge(indexed_clean.data);
    merge(indexed_dirty.data);
    auto clean_id = project.file_table.intern(Spelling::absolute(indexed_clean.tu_path));
    auto dirty_id = project.file_table.intern(Spelling::absolute(indexed_dirty.tu_path));

    auto save_body = [&]() -> kota::task<> {
        co_await async_save();
    };
    auto task = save_body();
    loop.schedule(task);
    loop.run();

    ZASSERT(!project.index.shard(clean_id));
    ZASSERT(project.index.shard(dirty_id) != nullptr);
    ZASSERT(pump.pending_reason(clean_id) == ReindexReason::ContentChanged);
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

    // The probe runs while the save awaits the search build on the thread
    // pool: what was merged since the last build is in neither index yet,
    // so it must still be pending, where a search scans it.
    auto save_body = [&]() -> kota::task<> {
        co_await fx.async_save(index::SearchRebuild::Idle);
    };
    bool findable = true;
    auto probe_body = [&]() -> kota::task<> {
        auto& table = project.index;
        for(auto hash: merged) {
            findable &=
                table.search_index().contains(hash) || table.search_pending().contains(hash);
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
        ZASSERT(project.index.search_index().contains(hash));
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
        return search.search(*index::SymbolQuery::parse(text), 10, project.index.workspace)
            .hits.size();
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
    bool on_disk = false;
    auto key = index::blob_key(project.file_table.resolve(header_id));
    project.database->for_each_key(index::BlobKind::Shard,
                                   [&](llvm::StringRef k) { on_disk |= k == key; });
    ZASSERT(!on_disk);
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
    bool on_disk = false;
    auto key = index::blob_key(project.file_table.resolve(header_id));
    project.database->for_each_key(index::BlobKind::Shard,
                                   [&](llvm::StringRef k) { on_disk |= k == key; });
    ZASSERT(!on_disk);

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
    struct FailingStorage final : index::BlobDatabase {
        index::ReadBlob read(index::BlobKind, llvm::StringRef) override {
            return {};
        }

        bool contains(index::BlobKind, llvm::StringRef) override {
            return false;
        }

        llvm::SmallVector<std::uint32_t> write(llvm::ArrayRef<Blob> puts,
                                               llvm::ArrayRef<index::BlobKey>) override {
            llvm::SmallVector<std::uint32_t> failed;
            for(std::uint32_t i = 0; i < puts.size(); i += 1) {
                failed.push_back(i);
            }
            return failed;
        }

        void for_each_key(index::BlobKind, llvm::function_ref<void(llvm::StringRef)>) override {}

        std::expected<std::uint64_t, std::string> advance_read_snapshot() override {
            return 1;
        }

        void retire_old_snapshot() override {}

        std::expected<bool, std::string> grow() override {
            return false;
        }
    };

    project.database = std::make_unique<FailingStorage>();

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

    open_store(tmp, project);
    save();
    ZASSERT(project.index.last_save_shards() == 1u);
    ZASSERT(project.index.pending_shard_writes() == 0u);
}

ZEST_CASE(WriteCorruptionRebuildsDatabase) {
    TempDir tmp;
    tmp.touch("clean.cpp", "int clean_value() { return 1; }\n");
    tmp.touch("dirty.cpp", "int dirty_value() { return 2; }\n");
    open_store(tmp, project);

    // Corruption showing up at write time (a damaged page only the write's
    // tree descent reaches): the save must condemn the environment and
    // continue on a fresh one instead of re-writing into it every save.
    // Batch shards own their bytes and stay to re-persist; the clean
    // resident view is shed and its owner re-enqueued.
    struct CorruptOnWrite final : index::BlobDatabase {
        bool* condemned;
        bool fail = false;
        bool poisoned = false;
        std::map<std::pair<index::BlobKind, std::string>, std::string> stored;

        index::ReadBlob read(index::BlobKind kind, llvm::StringRef key) override {
            auto it = stored.find({kind, key.str()});
            if(it == stored.end()) {
                return {};
            }
            return {.buffer = llvm::MemoryBuffer::getMemBufferCopy(it->second), .generation = 1};
        }

        bool contains(index::BlobKind kind, llvm::StringRef key) override {
            return stored.contains({kind, key.str()});
        }

        llvm::SmallVector<std::uint32_t> write(llvm::ArrayRef<Blob> puts,
                                               llvm::ArrayRef<index::BlobKey>) override {
            if(!fail) {
                for(auto& put: puts) {
                    stored[{put.kind, put.key}] = put.bytes;
                }
                return {};
            }
            poisoned = true;
            llvm::SmallVector<std::uint32_t> failed;
            for(std::uint32_t i = 0; i < puts.size(); i += 1) {
                failed.push_back(i);
            }
            return failed;
        }

        void for_each_key(index::BlobKind, llvm::function_ref<void(llvm::StringRef)>) override {}

        std::expected<std::uint64_t, std::string> advance_read_snapshot() override {
            return 1;
        }

        void retire_old_snapshot() override {}

        std::expected<bool, std::string> grow() override {
            return false;
        }

        bool corrupted() const override {
            return poisoned;
        }

        void condemn() override {
            *condemned = true;
        }
    };

    bool condemned = false;
    auto spy = std::make_unique<CorruptOnWrite>();
    spy->condemned = &condemned;
    auto* probe = spy.get();
    project.database = std::move(spy);

    auto indexed_clean = index_file(tmp, tmp.path("clean.cpp"));
    auto indexed_dirty = index_file(tmp, tmp.path("dirty.cpp"));
    ZASSERT(!indexed_clean.data.empty());
    ZASSERT(!indexed_dirty.data.empty());

    auto save = [&] {
        auto body = [&]() -> kota::task<> {
            co_await async_save();
        };
        auto task = body();
        loop.schedule(task);
        loop.run();
    };

    merge(indexed_clean.data);
    save();
    merge(indexed_dirty.data);
    probe->fail = true;
    save();

    ZASSERT(condemned);
    ZASSERT(project.database != nullptr);
    auto clean_id = project.file_table.intern(Spelling::absolute(indexed_clean.tu_path));
    auto dirty_id = project.file_table.intern(Spelling::absolute(indexed_dirty.tu_path));
    ZASSERT(!project.index.shard(clean_id));
    ZASSERT(project.index.shard(dirty_id) != nullptr);
    ZASSERT(pump.pending_reason(clean_id) == ReindexReason::ContentChanged);
    ZASSERT(project.index.last_save_shards() == 0u);

    // The next save re-persists everything servable into the fresh database.
    save();
    ZASSERT(project.index.last_save_shards() == 1u);
    ZASSERT(!has_unsaved_state(project));
}

ZEST_CASE(MigrationCorruptionRebuildsDatabase) {
    TempDir tmp;
    tmp.touch("main.cpp", "int migrate_value() { return 1; }\n");
    open_store(tmp, project);

    // Corruption showing up first at migration time (a damaged page only the
    // re-read from the advanced snapshot reaches, after the write-time
    // check passed): same recovery as write-time corruption — the resident
    // view is shed with its owner re-enqueued, the environment condemned
    // and replaced by a fresh one.
    struct CorruptOnRead final : index::BlobDatabase {
        bool* condemned;
        bool poisoned = false;

        index::ReadBlob read(index::BlobKind, llvm::StringRef) override {
            poisoned = true;
            return {};
        }

        bool contains(index::BlobKind, llvm::StringRef) override {
            return false;
        }

        llvm::SmallVector<std::uint32_t> write(llvm::ArrayRef<Blob>,
                                               llvm::ArrayRef<index::BlobKey>) override {
            return {};
        }

        void for_each_key(index::BlobKind, llvm::function_ref<void(llvm::StringRef)>) override {}

        std::expected<std::uint64_t, std::string> advance_read_snapshot() override {
            return 2;
        }

        void retire_old_snapshot() override {}

        std::expected<bool, std::string> grow() override {
            return false;
        }

        bool corrupted() const override {
            return poisoned;
        }

        void condemn() override {
            *condemned = true;
        }
    };

    bool condemned = false;
    auto spy = std::make_unique<CorruptOnRead>();
    spy->condemned = &condemned;
    project.database = std::move(spy);

    auto indexed = index_file(tmp, tmp.path("main.cpp"));
    ZASSERT(!indexed.data.empty());
    merge(indexed.data);

    auto save = [&] {
        auto body = [&]() -> kota::task<> {
            co_await async_save();
        };
        auto task = body();
        loop.schedule(task);
        loop.run();
    };
    save();

    auto path_id = project.file_table.intern(Spelling::absolute(indexed.tu_path));
    ZASSERT(condemned);
    ZASSERT(project.database != nullptr);
    ZASSERT(!project.index.shard(path_id));
    ZASSERT(pump.pending_reason(path_id) == ReindexReason::ContentChanged);
    ZASSERT(project.index.last_save_shards() == 0u);

    // The re-dirtied manifests, global and CDB snapshot re-persist into
    // the fresh database.
    save();
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
    ZASSERT(llvm::is_contained(log.writes[0], index::BlobKind::TUManifest));
    ZASSERT(!llvm::is_contained(log.writes[0], index::BlobKind::SearchIndex));
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
    {
        IndexerFixture f;
        open_store(tmp, f.project);
        auto indexed = index_file(tmp, tmp.path("a.cpp"));
        ZASSERT(f.merge(indexed.data));
        auto box_id = f.project.file_table.intern(Spelling::absolute(tmp.path("box.h")));
        auto resolutions = f.project.index.instantiation_shard(box_id)->resolutions();
        stored.assign(resolutions.begin(), resolutions.end());
        f.save();
    }

    IndexerFixture f;
    open_store(tmp, f.project);
    f.load();
    auto box_id = f.project.file_table.intern(Spelling::absolute(tmp.path("box.h")));
    auto* instantiations = f.project.index.instantiation_shard(box_id);
    ZASSERT(instantiations);
    ZEXPECT(std::ranges::equal(instantiations->resolutions(), stored));
    auto uses = f.project.index.live_uses(box_id);
    ZASSERT(uses.size() == 1u);
    ZEXPECT(instantiations->resolutions_of(uses.front()) == stored);
    ZEXPECT(
        !f.pump.pending_reason(f.project.file_table.intern(Spelling::absolute(tmp.path("a.cpp")))));
}

ZEST_CASE(UnservableInstantiationsReindex) {
    // A unit naming a use its file's blob cannot serve is indexed again:
    // nothing else would restore what it resolved.
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
    std::string box_key;
    {
        IndexerFixture f;
        open_store(tmp, f.project);
        auto indexed = index_file(tmp, tmp.path("a.cpp"));
        ZASSERT(f.merge(indexed.data));
        f.save();
        box_key = index::blob_key(f.project.file_table.resolve(
            f.project.file_table.intern(Spelling::absolute(tmp.path("box.h")))));
    }
    inject_blob(tmp, index::BlobKind::InstantiationShard, box_key, "corrupted beyond verification");

    IndexerFixture f;
    open_store(tmp, f.project);
    f.load();
    ZEXPECT(
        f.pump.pending_reason(f.project.file_table.intern(Spelling::absolute(tmp.path("a.cpp")))));
}

};  // ZEST_SUITE(ProjectIndexSave)

}  // namespace
}  // namespace clice::testing

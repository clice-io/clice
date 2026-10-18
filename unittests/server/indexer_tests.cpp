module;

#include "modules/prelude.h"
#ifndef _WIN32
#include <unistd.h>
#endif

module clice;

import :command.argument_parser;
import :compile.compilation;
import :config.config;
import :index.database;
import :index.format;
import :index.indexer;
import :index.instantiation_shard;
import :index.shard;
import :index.tu_index;
import :index.tu_manifest;
import :project.command_resolver;
import :project.index_store;
import :project.project;
import :sched.families.pch;
import :sched.families.pcm;
import :sched.families.turun;
import :sched.graph;
import :sched.index.pump;
import :sched.stack;
import :server.editor_context;
import :syntax.dependency_graph;
import :unittests.server.worker_test_helpers;
import :unittests.utils.cdb_helper;
import :unittests.utils.temp_dir;
import :unittests.utils.tu_index_mirror;
import :unittests.utils;
import :vfs.cache_store;
import :worker.pool;

namespace clice::testing {

/// Test fixture with friend access to the pump's and the store's
/// internals. The take wrappers (merge/save/load/drop_index) route each
/// store report back into the pump, as the production callers do.
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
    IndexStore index_store{loop, project, commands};
    TURunFamily turun{graph, project, commands, pcm, pch, index_store, pool};
    IndexPump pump{loop, project, turun, index_store, pool};

    IndexerFixture() {
        turun.register_runner();
    }

    /// Merge a worker result and take its report, as the TURun round does.
    bool merge(const void* data, std::size_t size) {
        auto report = index_store.merge(data, size, {});
        if(report) {
            pump.apply_report(*report);
        }
        return report.has_value();
    }

    /// One save with the pump's debt snapshot, its report taken back —
    /// without persist()'s retry, so a test observes each save.
    kota::task<> async_save(IndexStore::SearchRebuild search = IndexStore::SearchRebuild::Behind) {
        pump.apply_report(co_await index_store.save(pump.save_debt(), search));
    }

    /// Load and take the report, as the workspace load does. Returns the
    /// decode result (false = old-format or corrupt project table).
    bool load(bool read_only = false) {
        auto result = index_store.load({.read_only = read_only});
        pump.apply_report(result.report);
        return result.decoded;
    }

    void drop_index(Fid id) {
        pump.apply_report(index_store.drop_index(id));
    }

    /// Replace the database with a fresh one, as corruption recovery does.
    void reopen_database() {
        index_store.reopen_fresh_database();
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
        index_store.begin_round();
    }

    /// Judge staleness inside the current round (see begin_round).
    bool needs_reindex(llvm::StringRef path) {
        return index_store.needs_reindex(project.file_table.intern(Spelling::absolute(path)));
    }

    bool table_dirty() {
        return index_store.table_dirty;
    }

    /// Drop the merge's own dirty mark so a later assertion isolates what
    /// a check alone dirties.
    void reset_global_dirty() {
        index_store.table_dirty = false;
    }

    /// Record a standalone header's borrowed host, as the TURun round does
    /// after its merge commits (these tests merge worker results directly).
    void set_header_host(Fid header_id, Fid host_id) {
        index_store.record_header_host(header_id, host_id);
    }

    /// Run one save() to completion on the fixture's loop.
    void save(IndexStore::SearchRebuild search = IndexStore::SearchRebuild::Behind) {
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

namespace {

struct IndexedTU {
    std::string data;     ///< TU index bytes, as a worker would ship them.
    std::string tu_path;  ///< The TU's canonical path inside the index.
};

/// Index a real on-disk file in-process into its TU index bytes, the
/// sections of `known` variants left empty and the `known_instantiations`
/// not walked.
IndexedTU index_file(TempDir& tmp,
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

/// Re-encode a TU index with its consumed-content hash column dropped,
/// as a file behind a PCM ships it.
std::string strip_content_hashes(llvm::StringRef data) {
    auto mirror = TUIndexMirror::of(index::TUIndex::from_bytes(data));
    mirror.content_hashes.clear();
    return mirror.bytes();
}

/// A structurally valid blob of `text`'s content generation carrying an
/// explicit variant list — the shapes load()'s healing paths probe.
std::string planted_blob(llvm::StringRef text, std::uint64_t variant) {
    index::ShardBlob blob;
    blob.format_version = index::index_format_version;
    blob.content_hash = llvm::xxh3_64bits(text);
    blob.content_size = static_cast<std::uint32_t>(text.size());
    auto starts = kota::ipc::lsp::line_starts(std::string_view(text.data(), text.size()));
    for(std::size_t i = 0; i < starts.size(); i += 1) {
        auto next = i + 1 < starts.size() ? starts[i + 1] : blob.content_size;
        blob.line_lengths.push_back(static_cast<std::uint8_t>(next - starts[i]));
    }
    blob.variants = {variant};
    blob.symbol_hashes = {111};
    blob.relation_offsets = {0, 0};
    std::string bytes;
    llvm::raw_string_ostream os(bytes);
    index::serialize_blob(blob, os);
    return bytes;
}

void open_store(TempDir& tmp, Project& project) {
    auto store = CacheStore::open(tmp.path("cache"), 1);
    ZASSERT(store);
    project.store.emplace(std::move(*store));
    project.database = index::open_lmdb_database(*project.store, "");
}

/// Plant a blob between sessions (no fixture may be alive — the writer
/// lock), as the residue of a crash or a foreign writer.
void inject_blob(TempDir& tmp, index::BlobKind kind, llvm::StringRef key, std::string bytes) {
    auto store = CacheStore::open(tmp.path("cache"), 1);
    ZASSERT(store);
    auto db = index::open_lmdb_database(*store, "");
    ZASSERT(db != nullptr);
    index::BlobDatabase::Blob blob{kind, key.str(), std::move(bytes)};
    ZASSERT(db->write(blob, {}).empty());
}

/// The storage key of a file's shard or manifest blob (the database's naming).
std::string blob_key(llvm::StringRef path) {
    return std::format("{:016x}", llvm::xxh3_64bits(path));
}

ZEST_SUITE(IndexerMerge) {

IndexerFixture fx;
kota::event_loop& loop = fx.loop;
Project& project = fx.project;
IndexPump& pump = fx.pump;
IndexStore& index_store = fx.index_store;

bool merge(const void* data, std::size_t size) {
    return fx.merge(data, size);
}

kota::task<> async_save() {
    return fx.async_save();
}

bool load(bool read_only = false) {
    return fx.load(read_only);
}

void drop_index(Fid id) {
    fx.drop_index(id);
}

ZEST_CASE(MergeRejectsGarbage) {
    // A worker shipping corrupted bytes (torn write, stale format) must not
    // crash the master or leave partial state behind.
    ZASSERT(project.index.shards.empty());
    ZASSERT(project.index.symbol_count() == 0u);

    std::string garbage = "definitely not a flatbuffer, but long enough to try";
    ZASSERT(!merge(garbage.data(), garbage.size()));

    ZASSERT(project.index.shards.empty());
    ZASSERT(project.index.symbol_count() == 0u);
}

ZEST_CASE(SharedLocalNamesPrecompiledInput) {
    // An internal symbol a unit names in a file it holds no entries of is
    // rejected, unless the unit reads that file through its precompiled
    // header or a module: their own units wrote its entries.
    TempDir tmp;
    tmp.touch("shared.h", "");
    tmp.touch("main.cpp", R"(#include "shared.h"
int use() { return 1; }
)");
    auto indexed = index_file(tmp, tmp.path("main.cpp"));
    ZASSERT(!indexed.data.empty());
    auto mirror = TUIndexMirror::of(index::TUIndex::from_bytes(indexed.data));
    auto header_path = std::ranges::find_if(mirror.paths, [](const std::string& path) {
        return llvm::StringRef(path).ends_with("shared.h");
    });
    ZASSERT(header_path != mirror.paths.end());
    auto header_id = static_cast<std::uint32_t>(header_path - mirror.paths.begin());
    auto focus_id = static_cast<std::uint32_t>(mirror.paths.size() - 1);
    constexpr index::SymbolHash helper = ~0ull - 2;
    mirror.add_symbol(helper,
                      {.name = "helper", .scope = index::SymbolScope::Internal},
                      {header_id, focus_id});
    auto planted = mirror.bytes();

    ZEXPECT(!merge(planted.data(), planted.size()));

    auto header = project.file_table.intern(Spelling::absolute(*header_path));
    DepState input{.path_id = header, .version = project.file_table.intern_version(header, 1)};
    auto report = index_store.merge(planted.data(), planted.size(), {input});
    ZASSERT(report);
    pump.apply_report(*report);
    auto focus = project.file_table.intern(Spelling::absolute(mirror.paths.back()));
    llvm::SmallVector<Fid> reached;
    project.index.for_each_file_of_local(helper, header, [&](Fid file) {
        reached.push_back(file);
    });
    ZEXPECT(reached == llvm::SmallVector<Fid>{header, focus});

    // One the build saw gone still came out of the precompiled header.
    DepState gone{.path_id = header, .missing = true};
    ZEXPECT(index_store.merge(planted.data(), planted.size(), {gone}).has_value());
}

ZEST_CASE(MergeIgnoresDiskDrift) {
    TempDir tmp;
    tmp.touch("main.cpp", "int value() { return 1; }\n");
    auto src = tmp.path("main.cpp");

    auto indexed = index_file(tmp, src);
    ZASSERT(!indexed.data.empty());

    merge(indexed.data.data(), indexed.data.size());
    auto path_id = project.file_table.intern(Spelling::absolute(indexed.tu_path));
    auto it = project.index.shards.find(path_id);
    ZASSERT(it != project.index.shards.end());
    ZASSERT(it->second.content_hash() == llvm::xxh3_64bits("int value() { return 1; }\n"));

    // The disk moved on since the index was built. The blob is
    // self-contained — its entries pair with the generation it embeds, never
    // with the disk — so the re-merge is a pure variant hit and freshness
    // gating owns the drift.
    tmp.touch("main.cpp", "int renamed() { return 2; }\n");
    merge(indexed.data.data(), indexed.data.size());
    ZASSERT(it->second.content_hash() == llvm::xxh3_64bits("int value() { return 1; }\n"));
    ZASSERT(it->second.variants().size() == std::size_t(1));
    ZASSERT(project.index.contributions.lookup(path_id).contains(path_id));

    // An index built from the new content opens a new generation.
    auto fresh = index_file(tmp, src);
    ZASSERT(!fresh.data.empty());
    merge(fresh.data.data(), fresh.data.size());
    ZASSERT(it->second.content_hash() == llvm::xxh3_64bits("int renamed() { return 2; }\n"));
}

ZEST_CASE(SaveCommitsDirtyShard) {
    TempDir tmp;
    tmp.touch("main.cpp", "int flip_value() { return 1; }\n");
    auto src = tmp.path("main.cpp");
    open_store(tmp, project);

    auto indexed = index_file(tmp, src);
    ZASSERT(!indexed.data.empty());
    merge(indexed.data.data(), indexed.data.size());

    auto path_id = project.file_table.intern(Spelling::absolute(indexed.tu_path));
    ZASSERT(index_store.pending_shard_writes() == 1u);

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
    auto it = project.index.shards.find(path_id);
    ZASSERT(it != project.index.shards.end());
    ZASSERT(index_store.pending_shard_writes() == 0u);
    ZASSERT(index_store.last_save_shards() == 1u);
    ZASSERT(it->second.content_hash() == llvm::xxh3_64bits("int flip_value() { return 1; }\n"));
    ZASSERT(project.index.contributions.lookup(path_id).contains(path_id));
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

        llvm::SmallVector<std::size_t> write(llvm::ArrayRef<Blob> puts,
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
    merge(indexed.data.data(), indexed.data.size());
    auto path_id = project.file_table.intern(Spelling::absolute(indexed.tu_path));
    auto before = project.index.shards.find(path_id);
    ZASSERT(before != project.index.shards.end());
    auto variants_before = before->second.variants();
    const char* bytes_before = before->second.bytes().data();

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
    auto it = project.index.shards.find(path_id);
    ZASSERT(it != project.index.shards.end());
    ZASSERT(it->second.loaded());
    ZASSERT(it->second.bytes().data() != bytes_before);
    ZASSERT(it->second.content_hash() == llvm::xxh3_64bits("int migrate_value() { return 1; }\n"));
    ZASSERT(it->second.variants() == variants_before);
}

ZEST_CASE(GrowFailureShedsCleanShards) {
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

        llvm::SmallVector<std::size_t> write(llvm::ArrayRef<Blob> puts,
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
    spy->fail_key = blob_key(project.file_table.resolve(
        project.file_table.intern(Spelling::absolute(indexed_dirty.tu_path))));
    project.database = std::move(spy);

    merge(indexed_clean.data.data(), indexed_clean.data.size());
    merge(indexed_dirty.data.data(), indexed_dirty.data.size());
    auto clean_id = project.file_table.intern(Spelling::absolute(indexed_clean.tu_path));
    auto dirty_id = project.file_table.intern(Spelling::absolute(indexed_dirty.tu_path));

    auto save_body = [&]() -> kota::task<> {
        co_await async_save();
    };
    auto task = save_body();
    loop.schedule(task);
    loop.run();

    ZASSERT(!project.index.shards.contains(clean_id));
    ZASSERT(project.index.shards.contains(dirty_id));
    ZASSERT(pump.pending_reason(clean_id) == ReindexReason::ContentChanged);
}

ZEST_CASE(MidSaveMergeKept) {
    TempDir tmp;
    tmp.touch("main.cpp", "int first_value() { return 1; }\n");
    auto src = tmp.path("main.cpp");
    open_store(tmp, project);

    auto indexed = index_file(tmp, src);
    ZASSERT(!indexed.data.empty());
    merge(indexed.data.data(), indexed.data.size());
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
        mid_save_pending = index_store.pending_shard_writes();
        merge(fresh.data.data(), fresh.data.size());
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
    auto it = project.index.shards.find(path_id);
    ZASSERT(it != project.index.shards.end());
    ZASSERT(index_store.pending_shard_writes() == 1u);
    ZASSERT(it->second.content_hash() == llvm::xxh3_64bits("int second_value() { return 2; }\n"));

    auto again_body = [&]() -> kota::task<> {
        co_await async_save();
    };
    auto task = again_body();
    loop.schedule(task);
    loop.run();

    it = project.index.shards.find(path_id);
    ZASSERT(index_store.pending_shard_writes() == 0u);
    ZASSERT(it->second.content_hash() == llvm::xxh3_64bits("int second_value() { return 2; }\n"));
}

ZEST_CASE(RebuildKeepsPendingFindable) {
    TempDir tmp;
    tmp.touch("a.cpp", "int first_value() { return 1; }\n");
    tmp.touch("b.cpp", "int second_value() { return 2; }\n");
    open_store(tmp, project);
    auto first = index_file(tmp, tmp.path("a.cpp"));
    ZASSERT(!first.data.empty());
    merge(first.data.data(), first.data.size());
    fx.save(IndexStore::SearchRebuild::Idle);
    ZASSERT(project.index.search_index.loaded());

    auto second = index_file(tmp, tmp.path("b.cpp"));
    ZASSERT(!second.data.empty());
    merge(second.data.data(), second.data.size());
    llvm::SmallVector<index::SymbolHash> merged(project.index.search_pending.begin(),
                                                project.index.search_pending.end());
    ZASSERT(!merged.empty());

    // The probe runs while the save awaits the search build on the thread
    // pool: what was merged since the last build is in neither index yet,
    // so it must still be pending, where a search scans it.
    auto save_body = [&]() -> kota::task<> {
        co_await fx.async_save(IndexStore::SearchRebuild::Idle);
    };
    bool findable = true;
    auto probe_body = [&]() -> kota::task<> {
        auto& table = project.index;
        for(auto hash: merged) {
            findable &= table.search_index.contains(hash) || table.search_pending.contains(hash);
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
        ZASSERT(project.index.search_index.contains(hash));
        ZASSERT(!project.index.search_pending.contains(hash));
    }
}

ZEST_CASE(RebuildKeepsMidBuildChanges) {
    TempDir tmp;
    tmp.touch("a.cpp", "int first_value() { return 1; }\n");
    tmp.touch("b.cpp", "int second_value() { return 2; }\n");
    tmp.touch("c.cpp", "int second_value() { return 3; }\n");
    open_store(tmp, project);
    auto first = index_file(tmp, tmp.path("a.cpp"));
    ZASSERT(!first.data.empty());
    merge(first.data.data(), first.data.size());
    fx.save(IndexStore::SearchRebuild::Idle);

    auto second = index_file(tmp, tmp.path("b.cpp"));
    ZASSERT(!second.data.empty());
    merge(second.data.data(), second.data.size());
    llvm::SmallVector<index::SymbolHash> merged(project.index.search_pending.begin(),
                                                project.index.search_pending.end());
    ZASSERT(!merged.empty());
    auto moved = index_file(tmp, tmp.path("c.cpp"));
    ZASSERT(!moved.data.empty());

    // The definition moves to c.cpp while the build reads the table from
    // before: the built entry describes the old file, so the symbol stays
    // pending, where a search reads the table.
    auto save_body = [&]() -> kota::task<> {
        co_await fx.async_save(IndexStore::SearchRebuild::Idle);
    };
    auto merge_body = [&]() -> kota::task<> {
        merge(moved.data.data(), moved.data.size());
        co_return;
    };
    auto save_task = save_body();
    auto merge_task = merge_body();
    loop.schedule(save_task);
    loop.schedule(merge_task);
    loop.run();

    for(auto hash: merged) {
        ZASSERT(project.index.search_pending.contains(hash));
    }
}

ZEST_CASE(DroppedUnitPendsItsNames) {
    TempDir tmp;
    tmp.touch("main.cpp",
              "static int lonely_helper() { return 1; }\n"
              "int main() { return lonely_helper(); }\n");
    open_store(tmp, project);
    auto indexed = index_file(tmp, tmp.path("main.cpp"));
    ZASSERT(!indexed.data.empty());
    merge(indexed.data.data(), indexed.data.size());
    fx.save(IndexStore::SearchRebuild::Idle);
    ZASSERT(project.index.search_index.loaded());
    auto file = project.file_table.intern(Spelling::absolute(indexed.tu_path));
    ZASSERT(!project.index.search_pending_files.contains(file));

    // The built index still lists the static of the variant the drop
    // retires: searches must read the shard instead.
    drop_index(file);
    ZEXPECT(project.index.search_pending_files.contains(file));
}

ZEST_CASE(MergeHitWritesNothing) {
    TempDir tmp;
    tmp.touch("main.cpp", "int steady() { return 1; }\n");
    auto src = tmp.path("main.cpp");
    open_store(tmp, project);

    auto indexed = index_file(tmp, src);
    ZASSERT(!indexed.data.empty());
    merge(indexed.data.data(), indexed.data.size());
    auto save_body = [&]() -> kota::task<> {
        co_await async_save();
    };
    auto task = save_body();
    loop.schedule(task);
    loop.run();
    ZASSERT(index_store.pending_shard_writes() == 0u);

    // A re-merge whose variant the shard already stores is the steady state of
    // every background round: it must record contributions and touch no
    // blob at all.
    merge(indexed.data.data(), indexed.data.size());
    ZASSERT(index_store.pending_shard_writes() == 0u);
}

ZEST_CASE(SharedHeaderVariants) {
    TempDir tmp;
    tmp.touch("shared.h",
              "#pragma once\n#ifdef MODE\nint mode_fn();\n#endif\n"
              "inline int shared_fn() { return 1; }\n");
    tmp.touch("a.cpp", "#include \"shared.h\"\nint a() { return shared_fn(); }\n");
    tmp.touch("b.cpp", "#include \"shared.h\"\nint b() { return shared_fn(); }\n");

    auto a = index_file(tmp, tmp.path("a.cpp"));
    auto b = index_file(tmp, tmp.path("b.cpp"), {"-DMODE"});
    ZASSERT(!a.data.empty());
    ZASSERT(!b.data.empty());

    // Two TUs preprocess the header differently: both variants coexist in
    // one blob, each TU's contribution live.
    merge(a.data.data(), a.data.size());
    merge(b.data.data(), b.data.size());
    auto header_id = project.file_table.intern(Spelling::absolute(tmp.path("shared.h")));
    auto& shard = project.index.shards[header_id];
    ZASSERT(shard.variants().size() == std::size_t(2));
    ZASSERT(project.index.contributions.lookup(header_id).size() == std::size_t(2));

    // A third TU sharing a's preprocessing hits the stored variant: the
    // set does not grow, and neither existing contribution is disturbed.
    tmp.touch("c.cpp", "#include \"shared.h\"\nint c() { return shared_fn(); }\n");
    auto c = index_file(tmp, tmp.path("c.cpp"));
    ZASSERT(!c.data.empty());
    merge(c.data.data(), c.data.size());
    ZASSERT(shard.variants().size() == std::size_t(2));
    ZASSERT(project.index.contributions.lookup(header_id).size() == std::size_t(3));

    // Re-indexing a TU whose header variant is unchanged must not disturb
    // the other TUs' variants either.
    tmp.touch("a.cpp", "#include \"shared.h\"\nint a2() { return shared_fn(); }\n");
    auto fresh = index_file(tmp, tmp.path("a.cpp"));
    ZASSERT(!fresh.data.empty());
    merge(fresh.data.data(), fresh.data.size());
    ZASSERT(shard.variants().size() == std::size_t(2));
    ZASSERT(project.index.contributions.lookup(header_id).size() == std::size_t(3));
}

ZEST_CASE(KnownVariantByHash) {
    TempDir tmp;
    tmp.touch("shared.h", "#pragma once\ninline int shared_fn() { return 1; }\n");
    tmp.touch("a.cpp", "#include \"shared.h\"\nint a() { return shared_fn(); }\n");
    tmp.touch("b.cpp", "#include \"shared.h\"\nint b() { return shared_fn(); }\n");
    auto header_id = project.file_table.intern(Spelling::absolute(tmp.path("shared.h")));
    auto header_section = [&](const index::TUIndex& view) {
        std::uint32_t found = view.section_count();
        for(std::uint32_t i = 0; i < view.section_count(); i += 1) {
            if(llvm::sys::path::filename(view.path(view.section_path(i))) == "shared.h") {
                found = i;
            }
        }
        ZASSERT(found < view.section_count());
        return found;
    };

    auto a = index_file(tmp, tmp.path("a.cpp"));
    ZASSERT(!a.data.empty());
    auto a_view = index::TUIndex::from_bytes(a.data);
    auto variant = a_view.section_hash(header_section(a_view));

    // A result naming a variant the store does not hold commits nothing:
    // the file must run again.
    auto early = index_file(tmp, tmp.path("b.cpp"), {}, {variant});
    auto outdated = index_store.merge(early.data.data(), early.data.size(), {});
    ZASSERT(!outdated);
    ZEXPECT(outdated.error() == IndexStore::MergeError::Outdated);
    auto b_id = project.file_table.intern(Spelling::absolute(early.tu_path));
    ZEXPECT(!project.index.manifests.contains(b_id));

    // Once the store holds it, the bare hash counts as the file's
    // contribution like the bytes would.
    ZASSERT(merge(a.data.data(), a.data.size()));
    auto b =
        index_file(tmp, tmp.path("b.cpp"), {}, project.index.shards[header_id].variants());
    auto b_view = index::TUIndex::from_bytes(b.data);
    ZASSERT(b_view.section_blob(header_section(b_view)).empty());
    ZASSERT(merge(b.data.data(), b.data.size()));
    ZEXPECT(project.index.manifests.contains(b_id));
    ZEXPECT(project.index.shards[header_id].variants().size() == std::size_t(1));
    ZEXPECT(project.index.contributions.lookup(header_id).size() == std::size_t(2));
}

ZEST_CASE(KnownVariantsFromManifest) {
    // Without a scanned include graph, the files a TU's last manifest names
    // still offer their stored variants.
    TempDir tmp;
    tmp.touch("shared.h", "#pragma once\ninline int shared_fn() { return 1; }\n");
    tmp.touch("a.cpp", "#include \"shared.h\"\nint a() { return shared_fn(); }\n");
    auto a = index_file(tmp, tmp.path("a.cpp"));
    ZASSERT(merge(a.data.data(), a.data.size()));

    auto a_id = project.file_table.intern(Spelling::absolute(a.tu_path));
    auto header_id = project.file_table.intern(Spelling::absolute(tmp.path("shared.h")));
    std::vector<std::uint64_t> expected = project.index.shards[header_id].variants();
    llvm::append_range(expected, project.index.shards[a_id].variants());
    llvm::sort(expected);
    ZEXPECT(index_store.known_variants(a_id) == expected);

    auto other = project.file_table.intern(Spelling::absolute(tmp.path("other.cpp")));
    ZEXPECT(index_store.known_variants(other).empty());
}

ZEST_CASE(HeaderRegenerationReplaces) {
    TempDir tmp;
    tmp.touch("dep.h", "#pragma once\ninline int dep() { return 1; }\n");
    tmp.touch("main.cpp", "#include \"dep.h\"\nint use() { return dep(); }\n");
    auto src = tmp.path("main.cpp");

    auto v1 = index_file(tmp, src);
    ZASSERT(!v1.data.empty());
    merge(v1.data.data(), v1.data.size());
    auto header_id = project.file_table.intern(Spelling::absolute(tmp.path("dep.h")));
    auto tu_id = project.file_table.intern(Spelling::absolute(v1.tu_path));
    auto old_hash = project.index.contributions.lookup(header_id).lookup(tu_id);
    ZASSERT(old_hash != 0);

    // The header changes, a reindex captures it — and the header changes
    // AGAIN before the result merges. The worker's bytes are their own
    // generation: they merge verbatim regardless of the disk moving on, and
    // freshness gating owns the remaining drift.
    tmp.touch("dep.h", "#pragma once\ninline int dep() { return 2; }\n");
    auto v2 = index_file(tmp, src);
    ZASSERT(!v2.data.empty());
    tmp.touch("dep.h", "#pragma once\ninline int dep() { return 3; }\n");

    merge(v2.data.data(), v2.data.size());
    auto new_hash = project.index.contributions.lookup(header_id).lookup(tu_id);
    ZASSERT(new_hash != 0);
    ZASSERT(new_hash != old_hash);
    ZASSERT(project.index.shards[header_id].has_variant(new_hash));
    // A new content generation never shares variant storage with the old one.
    ZASSERT(!project.index.shards[header_id].has_variant(old_hash));
    ZASSERT(project.index.shards[header_id].content_hash() ==
            llvm::xxh3_64bits("#pragma once\ninline int dep() { return 2; }\n"));
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
    merge(a.data.data(), a.data.size());
    merge(b.data.data(), b.data.size());
    auto header_id = project.file_table.intern(Spelling::absolute(tmp.path("shared.h")));
    ZASSERT(project.index.shards[header_id].variants().size() == std::size_t(2));

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
    merge(b2.data.data(), b2.data.size());
    ZASSERT(project.index.shards[header_id].has_dead_variants());
    save();
    ZASSERT(project.index.shards[header_id].variants().size() == std::size_t(1));

    // a drops it too: no contribution is left, so the shard retires from
    // memory and from storage — with no owner left to re-enqueue.
    tmp.touch("a.cpp", "int a() { return 3; }\n");
    auto a2 = index_file(tmp, tmp.path("a.cpp"));
    ZASSERT(!a2.data.empty());
    merge(a2.data.data(), a2.data.size());
    save();
    ZASSERT(!project.index.shards.contains(header_id));
    ZASSERT(!pump.pending_reason(project.file_table.intern(Spelling::absolute(a2.tu_path)))
                 .has_value());
    bool on_disk = false;
    auto key = blob_key(project.file_table.resolve(header_id));
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
    merge(a.data.data(), a.data.size());
    merge(b.data.data(), b.data.size());
    auto header_id = project.file_table.intern(Spelling::absolute(tmp.path("pinned.h")));
    ZASSERT(project.index.shards[header_id].variants().size() == std::size_t(2));

    // The header moves to a new content generation and only pa catches up:
    // the blob starts over with pa's variant, while pb's manifest still
    // pins a hash the blob no longer stores.
    tmp.touch("pinned.h",
              "#pragma once\n#ifdef MODE\nint pin_mode();\n#endif\n"
              "inline int pin_fn() { return 2; }\n");
    auto a2 = index_file(tmp, tmp.path("pa.cpp"));
    ZASSERT(!a2.data.empty());
    merge(a2.data.data(), a2.data.size());
    ZASSERT(project.index.shards[header_id].variants().size() == std::size_t(1));

    // The rebuild re-enqueued pb; its pass then runs and fails, consuming
    // the slot — the state the retirement below must repair on its own.
    pump.clear_pending(project.file_table.intern(Spelling::absolute(b.tu_path)));

    // pa's index drops before pb reindexes: every stored variant is dead,
    // but pb's pinned hash keeps the live set nonempty. The save must
    // retire the shard rather than compact to an empty variant set.
    drop_index(project.file_table.intern(Spelling::absolute(a2.tu_path)));
    auto body = [&]() -> kota::task<> {
        co_await async_save();
    };
    auto task = body();
    loop.schedule(task);
    loop.run();

    ZASSERT(!project.index.shards.contains(header_id));
    bool on_disk = false;
    auto key = blob_key(project.file_table.resolve(header_id));
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

ZEST_CASE(RebuildRequeuesPinnedOwner) {
    TempDir tmp;
    tmp.touch("gen.h",
              "#pragma once\n#ifdef MODE\nint gen_mode();\n#endif\n"
              "inline int gen_fn() { return 1; }\n");
    tmp.touch("ga.cpp", "#include \"gen.h\"\nint ga() { return gen_fn(); }\n");
    tmp.touch("gb.cpp", "#include \"gen.h\"\nint gb() { return gen_fn(); }\n");

    auto a = index_file(tmp, tmp.path("ga.cpp"));
    auto b = index_file(tmp, tmp.path("gb.cpp"), {"-DMODE"});
    ZASSERT(!a.data.empty());
    ZASSERT(!b.data.empty());
    merge(a.data.data(), a.data.size());
    merge(b.data.data(), b.data.size());
    auto b_tu = project.file_table.intern(Spelling::absolute(b.tu_path));
    ZASSERT(!pump.pending_reason(b_tu).has_value());

    // The header moves to a new content generation and only ga catches up:
    // the rebuilt blob discards gb's variant. With no pending slot left for
    // gb, no in-process event would rebuild its variant — the rebuild itself
    // must re-enqueue it, and as ContentChanged: a reverted header reads
    // fresh by hash, which a deps-only slot would skip past.
    tmp.touch("gen.h",
              "#pragma once\n#ifdef MODE\nint gen_mode();\n#endif\n"
              "inline int gen_fn() { return 2; }\n");
    auto a2 = index_file(tmp, tmp.path("ga.cpp"));
    ZASSERT(!a2.data.empty());
    merge(a2.data.data(), a2.data.size());
    auto header_id = project.file_table.intern(Spelling::absolute(tmp.path("gen.h")));
    ZASSERT(project.index.shards[header_id].variants().size() == std::size_t(1));

    ZASSERT(pump.pending_reason(b_tu) == ReindexReason::ContentChanged);
    // ga's own fresh pin is stored: the rebuild must not re-enqueue it.
    ZASSERT(!pump.pending_reason(project.file_table.intern(Spelling::absolute(a2.tu_path)))
                 .has_value());
}

ZEST_CASE(RejectsCorruptSection) {
    TempDir tmp;
    tmp.touch("cor.h", "#pragma once\ninline int cor() { return 1; }\n");
    tmp.touch("cor_main.cpp", "#include \"cor.h\"\nint use_cor() { return cor(); }\n");

    auto indexed = index_file(tmp, tmp.path("cor_main.cpp"));
    ZASSERT(!indexed.data.empty());

    // Corrupt the focus file's blob bytes in place: the outer container still
    // verifies (sections are opaque bytes to it), only the blob's byte
    // identity check against the recorded section hash fails.
    std::string corrupt = indexed.data;
    auto tampered = index::TUIndex::from_bytes(corrupt);
    ZASSERT(tampered.loaded());
    auto focus_section = tampered.section_of(tampered.path_count() - 1);
    ZASSERT(focus_section);
    auto blob = tampered.section_blob(*focus_section);
    auto pos = llvm::StringRef(corrupt).find(blob);
    ZASSERT(pos != llvm::StringRef::npos);
    for(std::size_t i = 0; i < blob.size(); i += 1) {
        corrupt[pos + i] = 'X';
    }

    // The header section verifies fine and is staged before the focus
    // section's identity check fails; the reject must discard the whole
    // result — a manifest whose recorded versions all match the disk would
    // otherwise be judged fresh forever with the focus file's shard missing.
    merge(corrupt.data(), corrupt.size());
    auto tu_id = project.file_table.intern(Spelling::absolute(indexed.tu_path));
    auto header_id = project.file_table.intern(Spelling::absolute(tmp.path("cor.h")));
    ZASSERT(!project.index.manifests.contains(tu_id));
    ZASSERT(!project.index.shards.contains(header_id));
    // No global trace either: symbol identities from an untrusted result
    // would stay canonical for their hashes forever (later merges only
    // fill empty names), and stray FileVersions would persist with the
    // next save.
    ZASSERT(project.index.symbol_count() == 0u);
    ZASSERT(project.file_table.versions.empty());

    // The intact result still merges afterwards.
    merge(indexed.data.data(), indexed.data.size());
    ZASSERT(project.index.manifests.contains(tu_id));
    ZASSERT(project.index.shards.contains(header_id));
}

ZEST_CASE(HashlessRemergeHits) {
    TempDir tmp;
    tmp.touch("pcm.cpp", "int hashless_fn() { return 7; }\n");
    auto src = tmp.path("pcm.cpp");
    auto indexed = index_file(tmp, src);
    ZASSERT(!indexed.data.empty());

    // A file behind a PCM ships no consumed-content hash; the variant
    // identity is the blob's own byte hash, so membership needs no
    // content confirmation at all.
    auto stripped = strip_content_hashes(indexed.data);
    ZASSERT(!stripped.empty());

    merge(stripped.data(), stripped.size());
    auto path_id = project.file_table.intern(Spelling::absolute(src));
    ZASSERT(project.index.shards[path_id].variants().size() == std::size_t(1));

    // Re-merging the same variant must register as a hit, not append the
    // stored variant to the blob a second time.
    merge(stripped.data(), stripped.size());
    ZASSERT(project.index.shards[path_id].variants().size() == std::size_t(1));
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

        llvm::SmallVector<std::size_t> write(llvm::ArrayRef<Blob> puts,
                                             llvm::ArrayRef<index::BlobKey>) override {
            llvm::SmallVector<std::size_t> failed;
            for(std::size_t i = 0; i < puts.size(); i += 1) {
                failed.push_back(i);
            }
            return failed;
        }

        void for_each_key(index::BlobKind,
                          llvm::function_ref<void(llvm::StringRef)>) override {}

        std::expected<std::uint64_t, std::string> advance_read_snapshot() override {
            return 0;
        }

        void retire_old_snapshot() override {}

        std::expected<bool, std::string> grow() override {
            return false;
        }
    };

    project.database = std::make_unique<FailingStorage>();

    auto indexed = index_file(tmp, src);
    ZASSERT(!indexed.data.empty());
    merge(indexed.data.data(), indexed.data.size());
    ZASSERT(index_store.pending_shard_writes() == 1u);

    auto save = [&] {
        auto body = [&]() -> kota::task<> {
            co_await async_save();
        };
        auto task = body();
        loop.schedule(task);
        loop.run();
    };
    save();
    ZASSERT(index_store.last_save_shards() == 0u);
    // The failed batch is re-dirtied rather than discarded, so a later
    // save has it to retry and the cache converges once the storage
    // recovers.
    ZASSERT(index_store.pending_shard_writes() == 1u);

    open_store(tmp, project);
    save();
    ZASSERT(index_store.last_save_shards() == 1u);
    ZASSERT(index_store.pending_shard_writes() == 0u);
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

        index::ReadBlob read(index::BlobKind, llvm::StringRef) override {
            return {};
        }

        bool contains(index::BlobKind, llvm::StringRef) override {
            return false;
        }

        llvm::SmallVector<std::size_t> write(llvm::ArrayRef<Blob> puts,
                                             llvm::ArrayRef<index::BlobKey>) override {
            if(!fail) {
                return {};
            }
            poisoned = true;
            llvm::SmallVector<std::size_t> failed;
            for(std::size_t i = 0; i < puts.size(); i += 1) {
                failed.push_back(i);
            }
            return failed;
        }

        void for_each_key(index::BlobKind,
                          llvm::function_ref<void(llvm::StringRef)>) override {}

        std::expected<std::uint64_t, std::string> advance_read_snapshot() override {
            return 0;
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

    merge(indexed_clean.data.data(), indexed_clean.data.size());
    save();
    merge(indexed_dirty.data.data(), indexed_dirty.data.size());
    probe->fail = true;
    save();

    ZASSERT(condemned);
    ZASSERT(project.database != nullptr);
    auto clean_id = project.file_table.intern(Spelling::absolute(indexed_clean.tu_path));
    auto dirty_id = project.file_table.intern(Spelling::absolute(indexed_dirty.tu_path));
    ZASSERT(!project.index.shards.contains(clean_id));
    ZASSERT(project.index.shards.contains(dirty_id));
    ZASSERT(pump.pending_reason(clean_id) == ReindexReason::ContentChanged);
    ZASSERT(index_store.last_save_shards() == 0u);

    // The next save re-persists everything servable into the fresh database.
    save();
    ZASSERT(index_store.last_save_shards() == 1u);
    ZASSERT(!index_store.has_unsaved_state());
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

        llvm::SmallVector<std::size_t> write(llvm::ArrayRef<Blob>,
                                             llvm::ArrayRef<index::BlobKey>) override {
            return {};
        }

        void for_each_key(index::BlobKind,
                          llvm::function_ref<void(llvm::StringRef)>) override {}

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
    merge(indexed.data.data(), indexed.data.size());

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
    ZASSERT(!project.index.shards.contains(path_id));
    ZASSERT(pump.pending_reason(path_id) == ReindexReason::ContentChanged);
    ZASSERT(index_store.last_save_shards() == 0u);

    // The re-dirtied manifests, global and CDB snapshot re-persist into
    // the fresh database.
    save();
    ZASSERT(!index_store.has_unsaved_state());
}

};  // ZEST_SUITE(IndexerMerge)

ZEST_SUITE(IndexerStaleness) {

/// A merged TU with one header dependency, ready for staleness probing.
struct Indexed {
    TempDir tmp;
    IndexerFixture f;
    std::string src;
    std::string header;

    bool setup() {
        tmp.touch("dep.h", "#pragma once\ninline int dep() { return 1; }\n");
        tmp.touch("main.cpp", "#include \"dep.h\"\nint use() { return dep(); }\n");
        src = tmp.path("main.cpp");
        header = tmp.path("dep.h");
        // Age the files out of the mtime guard window so their stats can
        // confirm them, the way real project files predate an index run.
        if(!set_file_mtime(src, file_mtime_ns(src) - 10'000'000'000) ||
           !set_file_mtime(header, file_mtime_ns(header) - 10'000'000'000)) {
            return false;
        }
        auto indexed = index_file(tmp, src);
        if(indexed.data.empty()) {
            return false;
        }
        f.merge(indexed.data.data(), indexed.data.size());
        return true;
    }
};

ZEST_CASE(CreatedHeaderStales) {
    // Where a failed include looked is an input of the TU: a header
    // appearing there makes its index stale.
    TempDir tmp;
    tmp.touch("main.cpp", "#include \"gen.h\"\nint use() { return 0; }\n");
    auto src = tmp.path("main.cpp");
    auto indexed = index_file(tmp, src);
    ZASSERT(!indexed.data.empty());
    IndexerFixture f;
    f.merge(indexed.data.data(), indexed.data.size());
    auto tu = f.project.file_table.intern(Spelling::absolute(indexed.tu_path));
    auto generation = f.project.file_table.intern(Spelling::absolute(tmp.path("gen.h")));
    ZASSERT(f.project.index.absent_lookups.lookup(generation).contains(tu));
    ZASSERT(f.project.file_table.disk.seen_missing(generation));
    ZASSERT(!f.needs_reindex(src));

    tmp.touch("gen.h", "int make();\n");
    f.begin_round();
    ZASSERT(f.needs_reindex(src));
}

#ifndef _WIN32
ZEST_CASE(AbsentSpellingsOnePlace) {
    // A failed include looked in a directory under two spellings (one a
    // symlink): one place, recorded once, and a reindex drops it cleanly.
    TempDir tmp;
    tmp.touch("main.cpp", "#include \"gen.h\"\nint use() { return 0; }\n");
    tmp.mkdir("real");
    ZASSERT(::symlink(tmp.path("real").c_str(), tmp.path("link").c_str()) == 0);
    auto src = tmp.path("main.cpp");
    auto indexed = index_file(tmp, src, {"-I" + tmp.path("real"), "-I" + tmp.path("link")});
    ZASSERT(!indexed.data.empty());
    IndexerFixture f;
    f.merge(indexed.data.data(), indexed.data.size());
    f.merge(indexed.data.data(), indexed.data.size());
    auto tu = f.project.file_table.intern(Spelling::absolute(indexed.tu_path));
    auto& manifest = f.project.index.manifests.find(tu)->second;
    auto place = f.project.file_table.intern(Spelling::absolute(tmp.path("real/gen.h")));
    ZASSERT(llvm::count_if(manifest.absent, [&](VersionID version) {
                return f.project.file_table.version(version).fid == place;
            }) == 1);
}
#endif

ZEST_CASE(TouchStaysFresh) {
    Indexed x;
    ZASSERT(x.setup());
    ZASSERT(!x.f.needs_reindex(x.src));

    // Same bytes, new mtime: the stat fast path misses, the hash proves a
    // mere touch, and nothing persisted needs rewriting.
    ZASSERT(set_file_mtime(x.header, file_mtime_ns(x.header) + 5'000'000'000));
    x.f.reset_global_dirty();
    x.f.begin_round();
    ZASSERT(!x.f.needs_reindex(x.src));
    ZASSERT(!x.f.table_dirty());
}

ZEST_CASE(PreservedMtimeEditStale) {
    Indexed x;
    ZASSERT(x.setup());
    auto recorded = file_mtime_ns(x.header);

    // Different content restored to the recorded mtime (rsync -t, git
    // restore-mtime): equality of the stat is not enough — the size moved,
    // and the hash check must catch the edit.
    x.tmp.touch("dep.h", "#pragma once\ninline int dep() { return 12345; }\n");
    ZASSERT(set_file_mtime(x.header, recorded));
    x.f.begin_round();
    ZASSERT(x.f.needs_reindex(x.src));
}

ZEST_CASE(AllDepsChecked) {
    TempDir tmp;
    tmp.touch("first.h", "#pragma once\ninline int first() { return 1; }\n");
    tmp.touch("second.h", "#pragma once\ninline int second() { return 2; }\n");
    tmp.touch("main.cpp",
              "#include \"first.h\"\n#include \"second.h\"\n"
              "int use() { return first() + second(); }\n");
    IndexerFixture f;
    auto indexed = index_file(tmp, tmp.path("main.cpp"));
    ZASSERT(!indexed.data.empty());
    f.merge(indexed.data.data(), indexed.data.size());
    ZASSERT(!f.needs_reindex(tmp.path("main.cpp")));

    // Only the second dependency changes; a partial iteration would call
    // the TU fresh.
    auto recorded = file_mtime_ns(tmp.path("second.h"));
    tmp.touch("second.h", "#pragma once\ninline int second() { return 22222; }\n");
    ZASSERT(set_file_mtime(tmp.path("second.h"), recorded));
    f.begin_round();
    ZASSERT(f.needs_reindex(tmp.path("main.cpp")));
}

};  // ZEST_SUITE(IndexerStaleness)

ZEST_SUITE(IndexerLoad) {

ZEST_CASE(LoadRestoresIndex) {
    TempDir tmp;
    tmp.touch("dep.h", "#pragma once\ninline int dep() { return 1; }\n");
    tmp.touch("main.cpp", "#include \"dep.h\"\nint use() { return dep(); }\n");
    auto src = tmp.path("main.cpp");

    {
        IndexerFixture f;
        open_store(tmp, f.project);
        auto indexed = index_file(tmp, src);
        ZASSERT(!indexed.data.empty());
        f.merge(indexed.data.data(), indexed.data.size());
        f.save();
    }

    IndexerFixture f;
    open_store(tmp, f.project);
    f.load();

    auto tu_id = f.project.file_table.intern(Spelling::absolute(src));
    auto header_id = f.project.file_table.intern(Spelling::absolute(tmp.path("dep.h")));
    ZASSERT(f.project.index.shards.contains(tu_id));
    ZASSERT(f.project.index.shards.contains(header_id));
    ZASSERT(f.project.index.contributions.lookup(header_id).contains(tu_id));
    // The persisted versions make the untouched TU judge fresh without any
    // reindex.
    ZASSERT(!f.needs_reindex(src));
}

ZEST_CASE(IdleRebuildPinsSearch) {
    TempDir tmp;
    tmp.touch("main.cpp", "int use() { return 1; }\n");
    auto src = tmp.path("main.cpp");

    {
        IndexerFixture f;
        open_store(tmp, f.project);
        auto indexed = index_file(tmp, src);
        ZASSERT(!indexed.data.empty());
        f.merge(indexed.data.data(), indexed.data.size());
        // The plain save persists the table; the idle one then rebuilds
        // the search index with no other change to write.
        f.save();
        ZASSERT(!f.table_dirty());
        ZASSERT(!f.project.index.search_index.loaded());
        f.save(IndexStore::SearchRebuild::Idle);
        ZASSERT(f.project.index.search_index.loaded());
    }

    IndexerFixture f;
    open_store(tmp, f.project);
    f.load();
    ZASSERT(f.project.index.search_index.loaded());
}

ZEST_CASE(LoadHealsBrokenShard) {
    TempDir tmp;
    tmp.touch("dep.h", "#pragma once\ninline int dep() { return 1; }\n");
    tmp.touch("extra.h", "#pragma once\ninline int extra() { return 2; }\n");
    tmp.touch("main.cpp",
              "#include \"dep.h\"\n#include \"extra.h\"\n"
              "int use() { return dep() + extra(); }\n");
    auto src = tmp.path("main.cpp");
    std::string header_key;

    {
        IndexerFixture f;
        open_store(tmp, f.project);
        auto indexed = index_file(tmp, src);
        ZASSERT(!indexed.data.empty());
        f.merge(indexed.data.data(), indexed.data.size());
        f.save();
        header_key = blob_key(f.project.file_table.resolve(
            f.project.file_table.intern(Spelling::absolute(tmp.path("dep.h")))));
    }

    // Corrupt the header's blob and plant an orphan nothing references.
    inject_blob(tmp, index::BlobKind::Shard, header_key, "corrupted beyond verification");
    inject_blob(tmp, index::BlobKind::Shard, "deadbeefdeadbeef", "orphan");

    IndexerFixture f;
    open_store(tmp, f.project);
    f.load();

    // The header's shard is unservable, so its contributing TU's manifest
    // is dropped and the TU re-enqueued — no CDB entry would ever re-index
    // a header otherwise. The orphan is removed.
    auto tu_id = f.project.file_table.intern(Spelling::absolute(src));
    ZASSERT(f.project.index.manifests.empty());
    ZASSERT(f.pump.pending_reason(tu_id));

    // The dropped manifest also retired the TU's contribution to the
    // OTHER header: its loaded shard's live mask must follow, or it keeps
    // serving a variant nothing contributes any more.
    auto extra_id = f.project.file_table.intern(Spelling::absolute(tmp.path("extra.h")));
    auto extra_it = f.project.index.shards.find(extra_id);
    ZASSERT(extra_it != f.project.index.shards.end());
    ZASSERT(extra_it->second.has_dead_variants());

    // Load defers blob cleanup into the first save (no synchronous
    // database commits on the startup event loop); the orphan dies there.
    auto save_body = [&]() -> kota::task<> {
        co_await f.async_save();
    };
    auto task = save_body();
    f.loop.schedule(task);
    f.loop.run();
    bool orphan_alive = false;
    f.project.database->for_each_key(index::BlobKind::Shard, [&](llvm::StringRef key) {
        orphan_alive |= key == "deadbeefdeadbeef";
    });
    ZASSERT(!orphan_alive);
}

ZEST_CASE(ReadOnlyLoadKeepsDisk) {
    TempDir tmp;
    tmp.touch("dep.h", "#pragma once\ninline int dep() { return 1; }\n");
    tmp.touch("main.cpp", "#include \"dep.h\"\nint use() { return dep(); }\n");
    auto src = tmp.path("main.cpp");
    std::string header_key;
    std::string manifest_key;

    {
        IndexerFixture f;
        open_store(tmp, f.project);
        auto indexed = index_file(tmp, src);
        ZASSERT(!indexed.data.empty());
        f.merge(indexed.data.data(), indexed.data.size());
        f.save();
        header_key = blob_key(f.project.file_table.resolve(
            f.project.file_table.intern(Spelling::absolute(tmp.path("dep.h")))));
        manifest_key = blob_key(
            f.project.file_table.resolve(f.project.file_table.intern(Spelling::absolute(src))));
    }

    inject_blob(tmp, index::BlobKind::Shard, header_key, "corrupted beyond verification");
    inject_blob(tmp, index::BlobKind::Shard, "deadbeefdeadbeef", "orphan");

    IndexerFixture f;
    open_store(tmp, f.project);
    f.load(/*read_only=*/true);

    // The in-memory passes still run: the unservable header drops its
    // contributing TU's manifest and re-enqueues the TU.
    auto tu_id = f.project.file_table.intern(Spelling::absolute(src));
    ZASSERT(f.project.index.manifests.empty());
    ZASSERT(f.pump.pending_reason(tu_id));

    // But every blob survives on disk — a server running concurrently may
    // still reference what this reader judged stale.
    bool header_alive = false, orphan_alive = false, manifest_alive = false;
    f.project.database->for_each_key(index::BlobKind::Shard, [&](llvm::StringRef key) {
        header_alive |= key == header_key;
        orphan_alive |= key == "deadbeefdeadbeef";
    });
    f.project.database->for_each_key(index::BlobKind::TUManifest, [&](llvm::StringRef key) {
        manifest_alive |= key == manifest_key;
    });
    ZASSERT(header_alive);
    ZASSERT(orphan_alive);
    ZASSERT(manifest_alive);
}

ZEST_CASE(LoadHealsMissingVariant) {
    TempDir tmp;
    tmp.touch("dep.h", "#pragma once\ninline int dep() { return 1; }\n");
    tmp.touch("main.cpp", "#include \"dep.h\"\nint use() { return dep(); }\n");
    auto src = tmp.path("main.cpp");
    std::string header_key;

    {
        IndexerFixture f;
        open_store(tmp, f.project);
        auto indexed = index_file(tmp, src);
        ZASSERT(!indexed.data.empty());
        f.merge(indexed.data.data(), indexed.data.size());
        f.save();
        header_key = blob_key(f.project.file_table.resolve(
            f.project.file_table.intern(Spelling::absolute(tmp.path("dep.h")))));
    }

    // Replace the header's blob with one that verifies but stores a variant
    // no manifest contributed — the residue of a crash or failed write that
    // committed the manifest without its shard.
    inject_blob(tmp,
                index::BlobKind::Shard,
                header_key,
                planted_blob("#pragma once\ninline int dep() { return 1; }\n", 0x1234));

    IndexerFixture f;
    open_store(tmp, f.project);
    f.load();

    // set_live would silently drop the missing variant, so the shard is as
    // unservable as an unreadable one: the TU's manifest goes and the TU
    // re-enqueues.
    auto tu_id = f.project.file_table.intern(Spelling::absolute(src));
    ZASSERT(f.project.index.manifests.empty());
    ZASSERT(f.pump.pending_reason(tu_id));
}

ZEST_CASE(LoadHealsWrongGeneration) {
    TempDir tmp;
    tmp.touch("dep.h", "#pragma once\ninline int dep() { return 1; }\n");
    tmp.touch("main.cpp", "#include \"dep.h\"\nint use() { return dep(); }\n");
    auto src = tmp.path("main.cpp");
    std::string header_key;
    std::uint64_t variant = 0;

    {
        IndexerFixture f;
        open_store(tmp, f.project);
        auto indexed = index_file(tmp, src);
        ZASSERT(!indexed.data.empty());
        f.merge(indexed.data.data(), indexed.data.size());
        f.save();
        auto header_id = f.project.file_table.intern(Spelling::absolute(tmp.path("dep.h")));
        auto tu_id = f.project.file_table.intern(Spelling::absolute(src));
        variant = f.project.index.contributions.lookup(header_id).lookup(tu_id);
        ZASSERT(variant != 0);
        header_key = blob_key(f.project.file_table.resolve(header_id));
    }

    // Replace the header's blob with one from ANOTHER content generation
    // whose explicit variant list still reports the contributed identity —
    // the residue of a crash between shard and manifest writes. Every
    // recorded FileVersion matches the disk, so only the generation pin
    // can tell that positions would map through stale text.
    inject_blob(tmp, index::BlobKind::Shard, header_key, planted_blob("stale text", variant));

    IndexerFixture f;
    open_store(tmp, f.project);
    f.load();

    auto tu_id = f.project.file_table.intern(Spelling::absolute(src));
    ZASSERT(f.project.index.manifests.empty());
    ZASSERT(f.pump.pending_reason(tu_id));
}

ZEST_CASE(LoadDropsNewerManifest) {
    TempDir tmp;
    tmp.touch("main.cpp", "int lone() { return 1; }\n");
    auto src = tmp.path("main.cpp");

    {
        IndexerFixture f;
        open_store(tmp, f.project);
        auto indexed = index_file(tmp, src);
        ZASSERT(!indexed.data.empty());
        f.merge(indexed.data.data(), indexed.data.size());
        f.save();

        // Plant what a lost project table write leaves behind: a manifest carrying
        // a generation the persisted project table never reached. Every
        // FileVersion it references is known and its shard variant stored
        // (a shard-only reindex), so only the generation can tell that the
        // global's symbols never committed.
        auto tu_id = f.project.file_table.intern(Spelling::absolute(src));
        auto raced = f.project.index.manifests.find(tu_id)->second;
        raced.table_generation = f.project.index.table_generation + 1;
        std::string bytes;
        llvm::raw_string_ostream os(bytes);
        index::serialize_manifest(f.project.index.export_manifest(raced), os);
        // Keyed by the interned (canonical) spelling, like save() itself:
        // on Windows the raw TempDir spelling hashes to a different key.
        f.project.database->write(
            {
                {index::BlobKind::TUManifest,
                 blob_key(f.project.file_table.resolve(tu_id)),
                 std::move(bytes)}
        },
            {});
    }

    IndexerFixture f;
    open_store(tmp, f.project);
    f.load();

    // The raced manifest is dropped and its TU re-enqueued; the reindex
    // rewrites the manifest and the global together.
    auto tu_id = f.project.file_table.intern(Spelling::absolute(src));
    ZASSERT(f.project.index.manifests.empty());
    ZASSERT(f.pump.pending_reason(tu_id) == ReindexReason::ContentChanged);
}

ZEST_CASE(LoadDropsLostManifest) {
    TempDir tmp;
    tmp.touch("main.cpp", "int lone() { return 1; }\n");
    auto src = tmp.path("main.cpp");

    {
        IndexerFixture f;
        open_store(tmp, f.project);
        auto indexed = index_file(tmp, src);
        ZASSERT(!indexed.data.empty());
        f.merge(indexed.data.data(), indexed.data.size());
        f.save();

        // Plant what a failed manifest write under a committed global leaves
        // behind: the previous manifest, of an older generation, with every
        // FileVersion still resolvable and its shard variant stored (a
        // reindex that changed shards or the include tree only). Only the
        // global's pin can tell it is not the manifest the save meant.
        auto tu_id = f.project.file_table.intern(Spelling::absolute(src));
        auto lost = f.project.index.manifests.find(tu_id)->second;
        lost.table_generation = f.project.index.table_generation - 1;
        std::string bytes;
        llvm::raw_string_ostream os(bytes);
        index::serialize_manifest(f.project.index.export_manifest(lost), os);
        f.project.database->write(
            {
                {index::BlobKind::TUManifest,
                 blob_key(f.project.file_table.resolve(tu_id)),
                 std::move(bytes)}
        },
            {});
    }

    IndexerFixture f;
    open_store(tmp, f.project);
    f.load();

    // The wrong-generation manifest is dropped and the TU re-enqueued instead of
    // the previous reindex's dependency set and shards serving as current.
    auto tu_id = f.project.file_table.intern(Spelling::absolute(src));
    ZASSERT(f.project.index.manifests.empty());
    ZASSERT(f.pump.pending_reason(tu_id) == ReindexReason::ContentChanged);
}

ZEST_CASE(LoadRequeuesStaleManifest) {
    TempDir tmp;
    tmp.touch("dep.h", "#pragma once\ninline int dep() { return 1; }\n");
    tmp.touch("main.cpp", "#include \"dep.h\"\nint use() { return dep(); }\n");
    auto src = tmp.path("main.cpp");
    auto header = tmp.path("dep.h");

    {
        IndexerFixture f;
        open_store(tmp, f.project);
        auto indexed = index_file(tmp, src);
        ZASSERT(!indexed.data.empty());
        f.merge(indexed.data.data(), indexed.data.size());
        f.save();

        // Plant what a crash between save phases leaves: a manifest whose
        // dependency FileVersion the persisted project table never learned,
        // while the TU's own version is known — here for the header, whose
        // standalone index no CDB pass would ever rebuild.
        auto header_id = f.project.file_table.intern(Spelling::absolute(header));
        VersionID header_version;
        for(std::uint32_t version = 0; version < f.project.file_table.versions.size(); version += 1) {
            if(f.project.file_table.versions[version].fid == header_id) {
                header_version = VersionID{version};
            }
        }
        ZASSERT(header_version.valid());
        index::TUManifest stale;
        stale.tu_version = header_version;
        stale = f.project.index.export_manifest(stale);
        stale.nodes.push_back({.file = 9999});
        std::string bytes;
        llvm::raw_string_ostream os(bytes);
        index::serialize_manifest(stale, os);
        f.project.database->write(
            {
                {index::BlobKind::TUManifest, blob_key(header), std::move(bytes)}
        },
            {});
    }

    IndexerFixture f;
    open_store(tmp, f.project);
    f.load();

    // The unresolvable manifest is dropped and its TU re-enqueued instead
    // of losing its persisted index forever; the blob itself dies at the
    // first save (load defers cleanup off the startup event loop).
    auto header_id = f.project.file_table.intern(Spelling::absolute(header));
    ZASSERT(f.pump.pending_reason(header_id) == ReindexReason::ContentChanged);
    ZASSERT(!f.project.index.manifests.contains(header_id));
    auto save_body = [&]() -> kota::task<> {
        co_await f.async_save();
    };
    auto task = save_body();
    f.loop.schedule(task);
    f.loop.run();
    bool stale_alive = false;
    f.project.database->for_each_key(index::BlobKind::TUManifest, [&](llvm::StringRef key) {
        stale_alive |= key == blob_key(header);
    });
    ZASSERT(!stale_alive);

    // The TU whose manifest resolved is untouched.
    auto tu_id = f.project.file_table.intern(Spelling::absolute(src));
    ZASSERT(f.project.index.manifests.contains(tu_id));
    ZASSERT(!f.pump.pending_reason(tu_id).has_value());
}

ZEST_CASE(DeferredRemovalYieldsToFreshWrite) {
    TempDir tmp;
    tmp.touch("main.cpp", "int sweep_target() { return 1; }\n");
    auto src = tmp.path("main.cpp");

    {
        IndexerFixture f;
        open_store(tmp, f.project);
        auto indexed = index_file(tmp, src);
        ZASSERT(!indexed.data.empty());
        f.merge(indexed.data.data(), indexed.data.size());
        f.save();
        // The indexer keys blobs by the pool-canonical path, which need
        // not equal the raw temp path byte-for-byte (Windows 8.3 names).
        auto key = blob_key(f.project.file_table.resolve(
            f.project.file_table.intern(Spelling::absolute(indexed.tu_path))));
        // Replace the persisted manifest with an unresolvable one: the
        // next load removes it — deferred into the first save — and the
        // TU's shard turns orphan, deferred too.
        index::TUManifest stale;
        stale.tu_version = VersionID{999999};
        stale.nodes.push_back({.file = 9999});
        std::string bytes;
        llvm::raw_string_ostream os(bytes);
        index::serialize_manifest(stale, os);
        f.project.database->write(
            {
                {index::BlobKind::TUManifest, key, std::move(bytes)}
        },
            {});
    }

    IndexerFixture f;
    open_store(tmp, f.project);
    ZASSERT(f.load());

    // The removed TU re-indexes before the first save, so that save both
    // re-writes and (deferred) removes the same keys — the fresh write
    // must win or the file never persists again.
    auto indexed = index_file(tmp, src);
    ZASSERT(!indexed.data.empty());
    f.merge(indexed.data.data(), indexed.data.size());
    f.save();

    auto key = blob_key(f.project.file_table.resolve(
        f.project.file_table.intern(Spelling::absolute(indexed.tu_path))));
    bool manifest_alive = false;
    f.project.database->for_each_key(index::BlobKind::TUManifest,
                                     [&](llvm::StringRef k) { manifest_alive |= k == key; });
    bool shard_alive = false;
    f.project.database->for_each_key(index::BlobKind::Shard,
                                     [&](llvm::StringRef k) { shard_alive |= k == key; });
    ZASSERT(manifest_alive);
    ZASSERT(shard_alive);
}

ZEST_CASE(LmdbLoadServesAcrossSaves) {
    TempDir tmp;
    tmp.touch("main.cpp", "int lmdb_value() { return 1; }\n");
    auto src = tmp.path("main.cpp");

    auto open_lmdb = [&](Project& project) {
        auto store = CacheStore::open(tmp.path("cache"), 1);
        ZASSERT(store);
        project.store.emplace(std::move(*store));
        project.database = index::open_lmdb_database(*project.store, "");
        ZASSERT(project.database != nullptr);
    };

    {
        IndexerFixture f;
        open_lmdb(f.project);
        auto indexed = index_file(tmp, src);
        ZASSERT(!indexed.data.empty());
        f.merge(indexed.data.data(), indexed.data.size());
        f.save();
    }

    IndexerFixture f;
    open_lmdb(f.project);
    ZASSERT(f.load());
    auto path_id = f.project.file_table.intern(Spelling::absolute(src));
    ZASSERT(f.project.index.shards.contains(path_id));

    // The loaded shard borrows the open-time snapshot. A save that commits
    // anything advances and retires it — the shard must come out rebound
    // onto the fresh snapshot, still serving.
    tmp.touch("other.cpp", "int other_value() { return 2; }\n");
    auto other = index_file(tmp, tmp.path("other.cpp"));
    ZASSERT(!other.data.empty());
    f.merge(other.data.data(), other.data.size());
    f.save();

    auto it = f.project.index.shards.find(path_id);
    ZASSERT(it != f.project.index.shards.end());
    ZASSERT(it->second.loaded());
    ZASSERT(it->second.content_hash() == llvm::xxh3_64bits("int lmdb_value() { return 1; }\n"));
    ZASSERT(!it->second.bytes().empty());
}

ZEST_CASE(CorruptGlobalCondemnsDatabase) {
    // Page corruption under the project table: read fails, contains() still
    // says present, corrupted() confirms. load must condemn the database
    // (deleted on close) and continue on a fresh empty one instead of
    // parking in the disabled-persistence limbo forever.
    struct CorruptGlobal final : index::BlobDatabase {
        bool* condemned;

        index::ReadBlob read(index::BlobKind, llvm::StringRef) override {
            return {};
        }

        bool contains(index::BlobKind, llvm::StringRef) override {
            return true;
        }

        llvm::SmallVector<std::size_t> write(llvm::ArrayRef<Blob>,
                                             llvm::ArrayRef<index::BlobKey>) override {
            return {};
        }

        void for_each_key(index::BlobKind,
                          llvm::function_ref<void(llvm::StringRef)>) override {}

        std::expected<std::uint64_t, std::string> advance_read_snapshot() override {
            return 0;
        }

        void retire_old_snapshot() override {}

        std::expected<bool, std::string> grow() override {
            return false;
        }

        bool corrupted() const override {
            return true;
        }

        void condemn() override {
            *condemned = true;
        }
    };

    TempDir tmp;
    IndexerFixture f;
    open_store(tmp, f.project);
    bool condemned = false;
    auto spy = std::make_unique<CorruptGlobal>();
    spy->condemned = &condemned;
    f.project.database = std::move(spy);

    ZASSERT(f.load());
    ZASSERT(condemned);
    ZASSERT(f.project.database != nullptr);
}

ZEST_CASE(CorruptShardCondemnsDatabase) {
    TempDir tmp;
    tmp.touch("main.cpp", "int gone() { return 1; }\n");
    auto src = tmp.path("main.cpp");

    std::string tu_path;
    {
        IndexerFixture f;
        open_store(tmp, f.project);
        auto indexed = index_file(tmp, src);
        ZASSERT(!indexed.data.empty());
        f.merge(indexed.data.data(), indexed.data.size());
        tu_path = indexed.tu_path;
        f.save();
    }

    // Corruption latched past the global anchor: the shard read poisons
    // the latch while the project table stays readable. load must condemn here
    // too, and unwind everything it adopted — the views would otherwise
    // borrow from the condemned environment.
    struct CorruptShard final : index::BlobDatabase {
        std::unique_ptr<index::BlobDatabase> real;
        bool* condemned;
        bool poisoned = false;

        index::ReadBlob read(index::BlobKind kind, llvm::StringRef key) override {
            if(kind == index::BlobKind::Shard) {
                poisoned = true;
                return {};
            }
            return real->read(kind, key);
        }

        bool contains(index::BlobKind kind, llvm::StringRef key) override {
            return real->contains(kind, key);
        }

        llvm::SmallVector<std::size_t> write(llvm::ArrayRef<Blob> puts,
                                             llvm::ArrayRef<index::BlobKey> removes) override {
            return real->write(puts, removes);
        }

        void for_each_key(index::BlobKind kind,
                          llvm::function_ref<void(llvm::StringRef)> fn) override {
            real->for_each_key(kind, fn);
        }

        std::expected<std::uint64_t, std::string> advance_read_snapshot() override {
            return 0;
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

    IndexerFixture f;
    open_store(tmp, f.project);
    bool condemned = false;
    auto wrapper = std::make_unique<CorruptShard>();
    wrapper->real = std::move(f.project.database);
    wrapper->condemned = &condemned;
    f.project.database = std::move(wrapper);

    ZASSERT(f.load());
    ZASSERT(condemned);
    ZASSERT(f.project.index.shards.empty());
    ZASSERT(f.project.index.symbol_count() == 0u);

    // The TU has no CDB entry, so nothing else records the debt: it is
    // re-enqueued before the adopted state unwinds, and the fresh
    // database's first save persists it as standalone debt.
    ZASSERT(f.pump.pending_reason(f.project.file_table.intern(Spelling::absolute(tu_path))) ==
            ReindexReason::ContentChanged);
    ZASSERT(f.project.database != nullptr);
    f.save();
    auto snapshot = f.project.database->read(index::BlobKind::CommandSnapshot, "cdb");
    ZASSERT(snapshot);
    ZASSERT(snapshot.buffer->getBuffer().contains("main.cpp"));
}

ZEST_CASE(UnreadableGlobalPreserved) {
    TempDir tmp;
    tmp.touch("main.cpp", "int keep() { return 1; }\n");
    auto src = tmp.path("main.cpp");

    {
        IndexerFixture f;
        open_store(tmp, f.project);
        auto indexed = index_file(tmp, src);
        ZASSERT(!indexed.data.empty());
        f.merge(indexed.data.data(), indexed.data.size());
        f.save();
    }

    // The project table exists but fails to open — a transient IO error at
    // startup, not absence. Removing everything would destroy the intact
    // index; a fresh lineage saved over the unread one could alias its version
    // ids and generations. The session must run memory-only and leave every
    // blob for the next start.
    struct UnreadableGlobal final : index::BlobDatabase {
        std::unique_ptr<index::BlobDatabase> real;

        index::ReadBlob read(index::BlobKind kind, llvm::StringRef key) override {
            return kind == index::BlobKind::ProjectTable ? index::ReadBlob{} : real->read(kind, key);
        }

        bool contains(index::BlobKind kind, llvm::StringRef key) override {
            return real->contains(kind, key);
        }

        llvm::SmallVector<std::size_t> write(llvm::ArrayRef<Blob> puts,
                                             llvm::ArrayRef<index::BlobKey> removes) override {
            return real->write(puts, removes);
        }

        void for_each_key(index::BlobKind kind,
                          llvm::function_ref<void(llvm::StringRef)> fn) override {
            real->for_each_key(kind, fn);
        }

        std::expected<std::uint64_t, std::string> advance_read_snapshot() override {
            return 0;
        }

        void retire_old_snapshot() override {}

        std::expected<bool, std::string> grow() override {
            return false;
        }
    };

    {
        IndexerFixture f;
        open_store(tmp, f.project);
        auto wrapper = std::make_unique<UnreadableGlobal>();
        wrapper->real = std::move(f.project.database);
        f.project.database = std::move(wrapper);
        f.load();
        ZASSERT(f.project.index.manifests.empty());
        ZASSERT(f.project.database == nullptr);
    }

    IndexerFixture f;
    open_store(tmp, f.project);
    f.load();
    ZASSERT(!f.project.index.manifests.empty());
    ZASSERT(!f.project.index.shards.empty());
}

ZEST_CASE(DropIndexEvictsPersisted) {
    TempDir tmp;
    tmp.touch("dep.h", "#pragma once\ninline int dep() { return 1; }\n");
    tmp.touch("main.cpp", "#include \"dep.h\"\nint use() { return dep(); }\n");
    auto src = tmp.path("main.cpp");

    {
        IndexerFixture f;
        open_store(tmp, f.project);
        auto indexed = index_file(tmp, src);
        ZASSERT(!indexed.data.empty());
        f.merge(indexed.data.data(), indexed.data.size());
        f.save();

        // The compile command changed: content freshness cannot see it, so
        // the TU's index is dropped wholesale and staleness flips at once.
        f.drop_index(f.project.file_table.intern(Spelling::absolute(src)));
        ZASSERT(f.project.index.manifests.empty());
        ZASSERT(f.needs_reindex(src));
        f.save();
    }

    // The drop survives a restart: nothing on disk resurrects the
    // old-command index as fresh.
    IndexerFixture f;
    open_store(tmp, f.project);
    f.load();
    ZASSERT(f.project.index.manifests.empty());
    ZASSERT(f.project.index.shards.empty());
    ZASSERT(f.needs_reindex(src));
}

ZEST_CASE(OfflineCommandChangeReindexed) {
    TempDir tmp;
    tmp.touch("main.cpp", "int value() { return 1; }\n");
    auto src = tmp.path("main.cpp");

    {
        IndexerFixture f;
        open_store(tmp, f.project);
        f.project.cdb.add_command(tmp.root, src, llvm::StringRef("clang++ -DFOO=1 -c main.cpp"));
        auto indexed = index_file(tmp, src);
        ZASSERT(!indexed.data.empty());
        f.merge(indexed.data.data(), indexed.data.size());
        f.save();
    }

    // The command changed while no server ran: content freshness cannot
    // see it, so the persisted CDB snapshot must catch it at load.
    IndexerFixture f;
    open_store(tmp, f.project);
    f.project.cdb.add_command(tmp.root, src, llvm::StringRef("clang++ -DFOO=2 -c main.cpp"));
    f.load();

    auto tu_id = f.project.file_table.intern(Spelling::absolute(src));
    ZASSERT(!f.project.index.manifests.contains(tu_id));
    ZASSERT(f.pump.pending_reason(tu_id) == ReindexReason::ContentChanged);
}

ZEST_CASE(UnchangedCommandKept) {
    TempDir tmp;
    tmp.touch("main.cpp", "int value() { return 1; }\n");
    auto src = tmp.path("main.cpp");

    {
        IndexerFixture f;
        open_store(tmp, f.project);
        f.project.cdb.add_command(tmp.root, src, llvm::StringRef("clang++ -DFOO=1 -c main.cpp"));
        auto indexed = index_file(tmp, src);
        ZASSERT(!indexed.data.empty());
        f.merge(indexed.data.data(), indexed.data.size());
        f.save();
    }

    IndexerFixture f;
    open_store(tmp, f.project);
    f.project.cdb.add_command(tmp.root, src, llvm::StringRef("clang++ -DFOO=1 -c main.cpp"));
    f.load();

    auto tu_id = f.project.file_table.intern(Spelling::absolute(src));
    ZASSERT(f.project.index.manifests.contains(tu_id));
    ZASSERT(!f.pump.pending_reason(tu_id).has_value());
}

ZEST_CASE(RemovedEntryKeepsIndex) {
    TempDir tmp;
    tmp.touch("main.cpp", "int value() { return 1; }\n");
    auto src = tmp.path("main.cpp");

    {
        IndexerFixture f;
        open_store(tmp, f.project);
        f.project.cdb.add_command(tmp.root, src, llvm::StringRef("clang++ -DFOO=1 -c main.cpp"));
        auto indexed = index_file(tmp, src);
        ZASSERT(!indexed.data.empty());
        f.merge(indexed.data.data(), indexed.data.size());
        f.save();
    }

    // The entry vanished from the CDB: the last-known index still serves
    // navigation, same conservative semantics as the live reload path.
    IndexerFixture f;
    open_store(tmp, f.project);
    f.load();

    auto tu_id = f.project.file_table.intern(Spelling::absolute(src));
    ZASSERT(f.project.index.manifests.contains(tu_id));
    ZASSERT(!f.pump.pending_reason(tu_id).has_value());
}

ZEST_CASE(UndeclaredSourceRetires) {
    TempDir tmp;
    tmp.touch("main.cpp", "int value() { return 1; }\n");
    auto src = tmp.path("main.cpp");
    tmp.touch("a/compile_commands.json", "[]");
    tmp.touch("b/compile_commands.json",
              std::format(R"([{{"directory": "{}", "file": "main.cpp", )"
                          R"("arguments": ["clang++", "-c", "main.cpp"]}}])",
                          json_escape(tmp.root)));
    auto load_declared = [&](IndexerFixture& f, std::vector<std::string> databases) {
        ConfigRule rule;
        rule.compile_commands.assign(databases.begin(), databases.end());
        f.project.config.rules = {std::move(rule)};
        f.project.config.finalize(CanonicalPath(Spelling::absolute(tmp.root)));
        for(auto source: f.project.build.declared_sources()) {
            f.project.cdb.load(source);
        }
    };

    {
        IndexerFixture f;
        open_store(tmp, f.project);
        load_declared(f, {"a", "b"});
        auto indexed = index_file(tmp, src);
        ZASSERT(!indexed.data.empty());
        ZASSERT(f.merge(indexed.data.data(), indexed.data.size()));
        f.save();
    }

    // The configuration stopped declaring the database that listed the
    // file. Unlike a declared database that failed to load, a dropped
    // declaration is the build's final word: its index leaves.
    IndexerFixture f;
    open_store(tmp, f.project);
    load_declared(f, {"a"});
    f.load();

    auto tu_id = f.project.file_table.intern(Spelling::absolute(src));
    ZASSERT(!f.project.index.manifests.contains(tu_id));
    ZASSERT(!f.pump.pending_reason(tu_id).has_value());
}

ZEST_CASE(SourceRelocationPersists) {
    TempDir tmp;
    tmp.touch("main.cpp", "int value() { return 1; }\n");
    auto src = tmp.path("main.cpp");
    auto listing = std::format(R"([{{"directory": "{}", "file": "main.cpp", )"
                               R"("arguments": ["clang++", "-c", "main.cpp"]}}])",
                               json_escape(tmp.root));
    auto load_declared = [&](IndexerFixture& f) {
        ConfigRule rule;
        rule.compile_commands.assign({"a", "b"});
        f.project.config.rules = {std::move(rule)};
        f.project.config.finalize(CanonicalPath(Spelling::absolute(tmp.root)));
        for(auto source: f.project.build.declared_sources()) {
            f.project.cdb.load(source);
        }
    };

    {
        tmp.touch("a/compile_commands.json", listing);
        tmp.touch("b/compile_commands.json", "[]");
        IndexerFixture f;
        open_store(tmp, f.project);
        load_declared(f);
        auto indexed = index_file(tmp, src);
        ZASSERT(!indexed.data.empty());
        ZASSERT(f.merge(indexed.data.data(), indexed.data.size()));
        f.save();
    }

    // The file moved to the other database with the same command: the
    // provenance change alone reindexes, so the snapshot records the
    // database that now guarantees the index.
    tmp.touch("a/compile_commands.json", "[]");
    tmp.touch("b/compile_commands.json", listing);
    IndexerFixture f;
    open_store(tmp, f.project);
    load_declared(f);
    f.load();

    auto tu_id = f.project.file_table.intern(Spelling::absolute(src));
    ZASSERT(f.pump.pending_reason(tu_id));
}

ZEST_CASE(DiscoveredRelocationRetires) {
    TempDir tmp;
    tmp.touch("main.cpp", "int value() { return 1; }\n");
    tmp.touch("other.cpp", "int other() { return 2; }\n");
    auto src = tmp.path("main.cpp");
    auto listing = [&](llvm::StringRef file) {
        return std::format(R"([{{"directory": "{}", "file": "{}", )"
                           R"("arguments": ["clang++", "-c", "{}"]}}])",
                           json_escape(tmp.root),
                           file,
                           file);
    };

    {
        IndexerFixture f;
        open_store(tmp, f.project);
        f.project.config.finalize(CanonicalPath(Spelling::absolute(tmp.root)));
        tmp.touch("compile_commands.json", listing("main.cpp"));
        f.project.cdb.load(tmp.path("compile_commands.json"));
        auto indexed = index_file(tmp, src);
        ZASSERT(!indexed.data.empty());
        ZASSERT(f.merge(indexed.data.data(), indexed.data.size()));
        f.save();
    }

    // Discovery found the database elsewhere this session and loaded it:
    // shards only the vanished one guaranteed retire, like a dropped
    // declaration's.
    llvm::sys::fs::remove(tmp.path("compile_commands.json"));
    tmp.touch("build/compile_commands.json", listing("other.cpp"));
    IndexerFixture f;
    open_store(tmp, f.project);
    f.project.config.finalize(CanonicalPath(Spelling::absolute(tmp.root)));
    f.project.cdb.load(tmp.path("build/compile_commands.json"));
    f.load();

    auto tu_id = f.project.file_table.intern(Spelling::absolute(src));
    ZASSERT(!f.project.index.manifests.contains(tu_id));
}

ZEST_CASE(DefaultCommandKept) {
    TempDir tmp;
    tmp.touch("main.cpp", "int value() { return 1; }\n");
    auto src = tmp.path("main.cpp");
    auto add_default_rule = [&](IndexerFixture& f) {
        f.project.config.rules.push_back(ConfigRule{.default_command = std::string("clang++ -c")});
        f.project.config.finalize(CanonicalPath(Spelling::absolute(tmp.root)));
    };

    {
        IndexerFixture f;
        open_store(tmp, f.project);
        add_default_rule(f);
        auto indexed = index_file(tmp, src);
        ZASSERT(!indexed.data.empty());
        ZASSERT(f.merge(indexed.data.data(), indexed.data.size()));
        f.save();
    }

    // The same default command covers the file again: its recorded
    // identity matches and nothing is owed.
    IndexerFixture f;
    open_store(tmp, f.project);
    add_default_rule(f);
    f.load();

    auto tu_id = f.project.file_table.intern(Spelling::absolute(src));
    ZASSERT(f.project.index.manifests.contains(tu_id));
    ZASSERT(!f.pump.pending_reason(tu_id).has_value());
}

ZEST_CASE(UncoveredDefaultRetires) {
    TempDir tmp;
    tmp.touch("main.cpp", "int value() { return 1; }\n");
    auto src = tmp.path("main.cpp");

    {
        IndexerFixture f;
        open_store(tmp, f.project);
        f.project.config.rules.push_back(ConfigRule{.default_command = std::string("clang++ -c")});
        f.project.config.finalize(CanonicalPath(Spelling::absolute(tmp.root)));
        auto indexed = index_file(tmp, src);
        ZASSERT(!indexed.data.empty());
        ZASSERT(f.merge(indexed.data.data(), indexed.data.size()));
        f.save();
    }

    // The rule whose default command covered the file is gone and nothing
    // else compiles it: the index leaves instead of being rebuilt under the
    // builtin fallback, and no debt survives for a unit the build left.
    IndexerFixture f;
    open_store(tmp, f.project);
    f.load();

    auto tu_id = f.project.file_table.intern(Spelling::absolute(src));
    ZASSERT(!f.project.index.manifests.contains(tu_id));
    ZASSERT(!f.pump.pending_reason(tu_id).has_value());
}

ZEST_CASE(ExcludedRuleDropsIndex) {
    TempDir tmp;
    tmp.touch("main.cpp", "int value() { return 1; }\n");
    auto src = tmp.path("main.cpp");

    {
        IndexerFixture f;
        open_store(tmp, f.project);
        f.project.cdb.add_command(tmp.root, src, llvm::StringRef("clang++ -c main.cpp"));
        auto indexed = index_file(tmp, src);
        ZASSERT(!indexed.data.empty());
        ZASSERT(f.merge(indexed.data.data(), indexed.data.size()));
        f.save();
    }

    // A rule added since keeps the unit out of the index: the shards built
    // last session leave, and nothing re-enqueues them.
    IndexerFixture f;
    open_store(tmp, f.project);
    f.project.cdb.add_command(tmp.root, src, llvm::StringRef("clang++ -c main.cpp"));
    f.project.config.rules.push_back(ConfigRule{.patterns = {"**/*.cpp"}, .index = false});
    f.project.config.finalize(CanonicalPath(Spelling::absolute(tmp.root)));
    f.load();

    auto tu_id = f.project.file_table.intern(Spelling::absolute(src));
    ZASSERT(!f.project.index.manifests.contains(tu_id));
    ZASSERT(!f.pump.pending_reason(tu_id).has_value());
}

ZEST_CASE(RuleChangeReindexed) {
    TempDir tmp;
    tmp.touch("main.cpp", "int value() { return 1; }\n");
    auto src = tmp.path("main.cpp");

    {
        IndexerFixture f;
        open_store(tmp, f.project);
        f.project.cdb.add_command(tmp.root, src, llvm::StringRef("clang++ -c main.cpp"));
        auto indexed = index_file(tmp, src);
        ZASSERT(!indexed.data.empty());
        ZASSERT(f.merge(indexed.data.data(), indexed.data.size()));
        f.save();
    }

    // The CDB entry is unchanged, but a clice.toml rule now adjusts the
    // effective command — as invisible to content freshness as a command
    // edit, so the snapshot must cover it too.
    IndexerFixture f;
    open_store(tmp, f.project);
    f.project.cdb.add_command(tmp.root, src, llvm::StringRef("clang++ -c main.cpp"));
    f.project.config.rules.push_back(ConfigRule{
        .patterns = {"**/*.cpp"},
        .append = {"-DFOO=1"},
    });
    f.project.config.finalize(CanonicalPath(Spelling::absolute(tmp.root)));
    f.load();

    auto tu_id = f.project.file_table.intern(Spelling::absolute(src));
    ZASSERT(!f.project.index.manifests.contains(tu_id));
    ZASSERT(f.pump.pending_reason(tu_id) == ReindexReason::ContentChanged);
}

ZEST_CASE(HostChangeDropsHeader) {
    TempDir tmp;
    tmp.touch("dep.h", "#pragma once\ninline int dep() { return 1; }\n");
    tmp.touch("main.cpp", "#include \"dep.h\"\nint use() { return dep(); }\n");
    auto src = tmp.path("main.cpp");
    auto header = tmp.path("dep.h");

    {
        IndexerFixture f;
        open_store(tmp, f.project);
        f.project.cdb.add_command(tmp.root, src, llvm::StringRef("clang++ -DFOO=1 -c main.cpp"));
        // A standalone pass over the header, as a borrowed-context index
        // produces it; the host source itself was never indexed.
        auto indexed = index_file(tmp, header);
        ZASSERT(!indexed.data.empty());
        ZASSERT(f.merge(indexed.data.data(), indexed.data.size()));
        f.save();
    }

    // The host's command changed while no server ran: the header has no
    // CDB entry of its own, so only include reachability from the changed
    // source can catch its borrowed-command manifest.
    IndexerFixture f;
    open_store(tmp, f.project);
    f.project.cdb.add_command(tmp.root, src, llvm::StringRef("clang++ -DFOO=2 -c main.cpp"));
    auto src_id = f.project.file_table.intern(Spelling::absolute(src));
    auto header_id = f.project.file_table.intern(Spelling::absolute(header));
    f.project.dep_graph.set_includes(src_id, 0, {{header_id}});
    f.project.dep_graph.build_reverse_map();
    f.load();

    ZASSERT(!f.project.index.manifests.contains(header_id));
    ZASSERT(f.pump.pending_reason(header_id) == ReindexReason::ContentChanged);
}

ZEST_CASE(HeaderRuleChangeReindexed) {
    TempDir tmp;
    tmp.touch("dep.h", "#pragma once\ninline int dep() { return 1; }\n");
    auto header = tmp.path("dep.h");

    {
        IndexerFixture f;
        open_store(tmp, f.project);
        auto indexed = index_file(tmp, header);
        ZASSERT(!indexed.data.empty());
        ZASSERT(f.merge(indexed.data.data(), indexed.data.size()));
        f.save();
    }

    // A clice.toml rule matching the header itself changed offline: the
    // header has no CDB entry, so only its own snapshot entry can see it.
    IndexerFixture f;
    open_store(tmp, f.project);
    f.project.config.rules.push_back(ConfigRule{
        .patterns = {"**/*.h"},
        .append = {"-DFOO=1"},
    });
    f.project.config.finalize(CanonicalPath(Spelling::absolute(tmp.root)));
    f.load();

    auto header_id = f.project.file_table.intern(Spelling::absolute(header));
    ZASSERT(!f.project.index.manifests.contains(header_id));
    ZASSERT(f.pump.pending_reason(header_id) == ReindexReason::ContentChanged);
}

ZEST_CASE(RecordedHostChangeDrops) {
    TempDir tmp;
    tmp.touch("dep.h", "#pragma once\ninline int dep() { return 1; }\n");
    tmp.touch("main.cpp", "int use() { return 0; }\n");
    auto src = tmp.path("main.cpp");
    auto header = tmp.path("dep.h");

    {
        IndexerFixture f;
        open_store(tmp, f.project);
        f.project.cdb.add_command(tmp.root, src, llvm::StringRef("clang++ -DFOO=1 -c main.cpp"));
        auto indexed = index_file(tmp, header);
        ZASSERT(!indexed.data.empty());
        ZASSERT(f.merge(indexed.data.data(), indexed.data.size()));
        f.set_header_host(f.project.file_table.intern(Spelling::absolute(header)),
                          f.project.file_table.intern(Spelling::absolute(src)));
        f.save();
    }

    // The host's command changed offline AND the new command no longer
    // includes the header, so the rebuilt include graph cannot reach it —
    // only the recorded host association can catch the stale borrow.
    IndexerFixture f;
    open_store(tmp, f.project);
    f.project.cdb.add_command(tmp.root, src, llvm::StringRef("clang++ -DFOO=2 -c main.cpp"));
    f.load();

    auto header_id = f.project.file_table.intern(Spelling::absolute(header));
    ZASSERT(!f.project.index.manifests.contains(header_id));
    ZASSERT(f.pump.pending_reason(header_id) == ReindexReason::ContentChanged);
}

ZEST_CASE(ExcludedHostChangeDrops) {
    TempDir tmp;
    tmp.touch("dep.h", "#pragma once\ninline int dep() { return 1; }\n");
    tmp.touch("main.cpp", "#include \"dep.h\"\nint use() { return dep(); }\n");
    auto src = tmp.path("main.cpp");
    auto header = tmp.path("dep.h");
    auto add_default_rule = [&](IndexerFixture& f, std::string command) {
        f.project.config.rules.push_back(ConfigRule{.patterns = {"**/*.cpp"},
                                                    .default_command = std::move(command),
                                                    .index = false});
        f.project.config.finalize(CanonicalPath(Spelling::absolute(tmp.root)));
    };

    {
        IndexerFixture f;
        open_store(tmp, f.project);
        add_default_rule(f, "clang++ -DFOO=1 -c");
        auto indexed = index_file(tmp, header);
        ZASSERT(!indexed.data.empty());
        ZASSERT(f.merge(indexed.data.data(), indexed.data.size()));
        f.set_header_host(f.project.file_table.intern(Spelling::absolute(header)),
                          f.project.file_table.intern(Spelling::absolute(src)));
        f.save();
    }

    // The host compiles under a rule's default command and stays out of
    // the index itself, so no entry of its own records that command: the
    // header's snapshot must carry it for the offline change to be seen,
    // even while the host still includes the header.
    IndexerFixture f;
    open_store(tmp, f.project);
    add_default_rule(f, "clang++ -DFOO=2 -c");
    auto src_id = f.project.file_table.intern(Spelling::absolute(src));
    auto header_id = f.project.file_table.intern(Spelling::absolute(header));
    f.project.dep_graph.set_includes(src_id, 0, {{header_id}});
    f.project.dep_graph.build_reverse_map();
    f.load();

    ZASSERT(!f.project.index.manifests.contains(header_id));
    ZASSERT(f.pump.pending_reason(header_id) == ReindexReason::ContentChanged);
}

ZEST_CASE(PinnedHostKeepsHeader) {
    TempDir tmp;
    tmp.touch("dep.h", "#pragma once\ninline int dep() { return 1; }\n");
    tmp.touch("main.cpp", "#include \"dep.h\"\nint use() { return dep(); }\n");
    tmp.touch("other.cpp", "#include \"dep.h\"\nint more() { return dep(); }\n");
    auto src = tmp.path("main.cpp");
    auto other = tmp.path("other.cpp");
    auto header = tmp.path("dep.h");

    {
        IndexerFixture f;
        open_store(tmp, f.project);
        f.project.cdb.add_command(tmp.root, src, llvm::StringRef("clang++ -c main.cpp"));
        f.project.cdb.add_command(tmp.root, other, llvm::StringRef("clang++ -c other.cpp"));
        auto indexed = index_file(tmp, header);
        ZASSERT(!indexed.data.empty());
        ZASSERT(f.merge(indexed.data.data(), indexed.data.size()));
        f.set_header_host(f.project.file_table.intern(Spelling::absolute(header)),
                          f.project.file_table.intern(Spelling::absolute(src)));
        f.save();
    }

    // Another includer's command changed, but the header borrowed the
    // unchanged host's — the recorded association beats the reachability
    // approximation's over-drop.
    IndexerFixture f;
    open_store(tmp, f.project);
    f.project.cdb.add_command(tmp.root, src, llvm::StringRef("clang++ -c main.cpp"));
    f.project.cdb.add_command(tmp.root, other, llvm::StringRef("clang++ -DBAR=1 -c other.cpp"));
    auto src_id = f.project.file_table.intern(Spelling::absolute(src));
    auto other_id = f.project.file_table.intern(Spelling::absolute(other));
    auto header_id = f.project.file_table.intern(Spelling::absolute(header));
    f.project.dep_graph.set_includes(src_id, 0, {{header_id}});
    f.project.dep_graph.set_includes(other_id, 0, {{header_id}});
    f.project.dep_graph.build_reverse_map();
    f.load();

    ZASSERT(f.project.index.manifests.contains(header_id));
    ZASSERT(!f.pump.pending_reason(header_id).has_value());
}

ZEST_CASE(UnreachableHostRebuilds) {
    TempDir tmp;
    tmp.touch("dep.h", "#pragma once\ninline int dep() { return 1; }\n");
    tmp.touch("main.cpp", "int use() { return 0; }\n");
    auto src = tmp.path("main.cpp");
    auto header = tmp.path("dep.h");

    {
        IndexerFixture f;
        open_store(tmp, f.project);
        f.project.cdb.add_command(tmp.root, src, llvm::StringRef("clang++ -c main.cpp"));
        auto indexed = index_file(tmp, header);
        ZASSERT(!indexed.data.empty());
        ZASSERT(f.merge(indexed.data.data(), indexed.data.size()));
        f.set_header_host(f.project.file_table.intern(Spelling::absolute(header)),
                          f.project.file_table.intern(Spelling::absolute(src)));
        f.save();
    }

    // The host's command is untouched but an offline edit removed its
    // include of the header: the index keeps serving while a queued rebuild
    // re-selects a host.
    IndexerFixture f;
    open_store(tmp, f.project);
    f.project.cdb.add_command(tmp.root, src, llvm::StringRef("clang++ -c main.cpp"));
    f.load();

    auto header_id = f.project.file_table.intern(Spelling::absolute(header));
    ZASSERT(f.project.index.manifests.contains(header_id));
    ZASSERT(f.pump.pending_reason(header_id) == ReindexReason::ContentChanged);
}

ZEST_CASE(CDBWriteFailureRetried) {
    TempDir tmp;
    tmp.touch("main.cpp", "int value() { return 1; }\n");
    auto src = tmp.path("main.cpp");

    // A storage that fails only the CDB snapshot blob, with everything
    // else committing normally.
    struct CDBFailingStorage final : index::BlobDatabase {
        std::unique_ptr<index::BlobDatabase> real;
        bool fail_cdb = true;
        llvm::SmallVector<index::BlobKind> written;

        index::ReadBlob read(index::BlobKind kind, llvm::StringRef key) override {
            return real->read(kind, key);
        }

        bool contains(index::BlobKind kind, llvm::StringRef key) override {
            return real->contains(kind, key);
        }

        llvm::SmallVector<std::size_t> write(llvm::ArrayRef<Blob> puts,
                                             llvm::ArrayRef<index::BlobKey> removes) override {
            llvm::SmallVector<std::size_t> failed;
            for(std::size_t i = 0; i < puts.size(); i += 1) {
                written.push_back(puts[i].kind);
                if(fail_cdb && puts[i].kind == index::BlobKind::CommandSnapshot) {
                    failed.push_back(i);
                } else if(!real->write(llvm::ArrayRef(puts[i]), {}).empty()) {
                    failed.push_back(i);
                }
            }
            real->write({}, removes);
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
            return real->grow();
        }
    };

    IndexerFixture f;
    open_store(tmp, f.project);
    f.project.cdb.add_command(tmp.root, src, llvm::StringRef("clang++ -c main.cpp"));
    auto failing = std::make_unique<CDBFailingStorage>();
    failing->real = std::move(f.project.database);
    auto* storage = failing.get();
    f.project.database = std::move(failing);

    auto indexed = index_file(tmp, src);
    ZASSERT(!indexed.data.empty());
    ZASSERT(f.merge(indexed.data.data(), indexed.data.size()));
    f.save();

    // The snapshot rides the batch behind the index state it describes.
    ZASSERT(int(storage->written.back()) == int(index::BlobKind::CommandSnapshot));
    ZASSERT(!storage->real->contains(index::BlobKind::CommandSnapshot, "cdb"));

    // Nothing else is dirty any more, yet the failed snapshot alone must
    // drive the next save until it commits — and until it does, the state
    // counts as unsaved (`clice index` fails on it after the final save).
    ZASSERT(f.index_store.has_unsaved_state());
    storage->fail_cdb = false;
    f.save();
    ZASSERT(storage->real->contains(index::BlobKind::CommandSnapshot, "cdb"));
    ZASSERT(!f.index_store.has_unsaved_state());
}

ZEST_CASE(MissingSnapshotRewritten) {
    TempDir tmp;
    tmp.touch("main.cpp", "int value() { return 1; }\n");
    auto src = tmp.path("main.cpp");

    {
        IndexerFixture f;
        open_store(tmp, f.project);
        f.project.cdb.add_command(tmp.root, src, llvm::StringRef("clang++ -c main.cpp"));
        auto indexed = index_file(tmp, src);
        ZASSERT(!indexed.data.empty());
        ZASSERT(f.merge(indexed.data.data(), indexed.data.size()));
        f.save();
        // The project table committed but the final CDB write never did: the rest of
        // the index is intact.
        f.project.database->write(
            {
        },
            {{index::BlobKind::CommandSnapshot, "cdb"}});
    }

    // A rerun that dirties nothing must still recreate the baseline —
    // without it, every later offline command edit would go undetected.
    IndexerFixture f;
    open_store(tmp, f.project);
    f.project.cdb.add_command(tmp.root, src, llvm::StringRef("clang++ -c main.cpp"));
    f.load();
    ZASSERT(f.index_store.has_unsaved_state());
    f.save();
    ZASSERT(f.project.database->contains(index::BlobKind::CommandSnapshot, "cdb"));
    ZASSERT(!f.index_store.has_unsaved_state());
}

ZEST_CASE(DroppedHeaderDebtRetried) {
    TempDir tmp;
    tmp.touch("dep.h", "#pragma once\ninline int dep() { return 1; }\n");
    tmp.touch("main.cpp", "int use() { return 0; }\n");
    auto src = tmp.path("main.cpp");
    auto header = tmp.path("dep.h");

    {
        IndexerFixture f;
        open_store(tmp, f.project);
        f.project.cdb.add_command(tmp.root, src, llvm::StringRef("clang++ -DFOO=1 -c main.cpp"));
        auto indexed = index_file(tmp, header);
        ZASSERT(!indexed.data.empty());
        ZASSERT(f.merge(indexed.data.data(), indexed.data.size()));
        f.set_header_host(f.project.file_table.intern(Spelling::absolute(header)),
                          f.project.file_table.intern(Spelling::absolute(src)));
        f.save();
    }

    {
        // The host's command changed offline: the header's index is dropped
        // and queued — but the rebuild never finishes this session. The saved
        // snapshot must keep recording the header, or nothing would ever
        // retry it.
        IndexerFixture f;
        open_store(tmp, f.project);
        f.project.cdb.add_command(tmp.root, src, llvm::StringRef("clang++ -DFOO=2 -c main.cpp"));
        f.load();
        auto header_id = f.project.file_table.intern(Spelling::absolute(header));
        ZASSERT(!f.project.index.manifests.contains(header_id));
        ZASSERT(f.pump.pending_reason(header_id) == ReindexReason::ContentChanged);
        f.save();
    }

    IndexerFixture f;
    open_store(tmp, f.project);
    f.project.cdb.add_command(tmp.root, src, llvm::StringRef("clang++ -DFOO=2 -c main.cpp"));
    f.load();

    auto header_id = f.project.file_table.intern(Spelling::absolute(header));
    ZASSERT(!f.project.index.manifests.contains(header_id));
    ZASSERT(f.pump.pending_reason(header_id) == ReindexReason::ContentChanged);
}

ZEST_CASE(VanishedHeaderDebtDies) {
    TempDir tmp;
    tmp.touch("dep.h", "#pragma once\ninline int dep() { return 1; }\n");
    tmp.touch("main.cpp", "int use() { return 0; }\n");
    auto src = tmp.path("main.cpp");
    auto header = tmp.path("dep.h");

    {
        IndexerFixture f;
        open_store(tmp, f.project);
        f.project.cdb.add_command(tmp.root, src, llvm::StringRef("clang++ -DFOO=1 -c main.cpp"));
        auto indexed = index_file(tmp, header);
        ZASSERT(!indexed.data.empty());
        ZASSERT(f.merge(indexed.data.data(), indexed.data.size()));
        f.set_header_host(f.project.file_table.intern(Spelling::absolute(header)),
                          f.project.file_table.intern(Spelling::absolute(src)));
        f.save();
    }

    {
        IndexerFixture f;
        open_store(tmp, f.project);
        f.project.cdb.add_command(tmp.root, src, llvm::StringRef("clang++ -DFOO=2 -c main.cpp"));
        f.load();
        f.save();
    }

    // The header was deleted while its debt entry sat in the snapshot: a
    // retry could never succeed, so the debt dies instead of keeping every
    // later run partial forever.
    ZASSERT(!llvm::sys::fs::remove(header));
    IndexerFixture f;
    open_store(tmp, f.project);
    f.project.cdb.add_command(tmp.root, src, llvm::StringRef("clang++ -DFOO=2 -c main.cpp"));
    f.load();
    ZASSERT(!f.pump.pending_reason(f.project.file_table.intern(Spelling::absolute(header)))
                 .has_value());
}

ZEST_CASE(StaleFormatDropsPch) {
    // A .pch.idx index written under an older index format is
    // unreadable; the format gate drops every PCH entry at load so the
    // pairs rebuild immediately, instead of the mismatch showing up lazily
    // on the first overlay query — which cannot trigger a rebuild.
    TempDir tmp;
    tmp.touch("dep.h", "int x;\n");
    auto dep_path = tmp.path("dep.h");
    std::string artifacts;

    auto setup = [&](IndexerFixture& f) {
        open_store(tmp, f.project);
        f.project.store->register_namespace(
            {.name = "pch", .extension = ".pch", .aux_extension = ".pch.idx"});
    };

    {
        IndexerFixture f;
        setup(f);
        auto pending = f.project.store->begin_store("pch", "k");
        ZASSERT(!vfs::write(pending.tmp_path, "pch-bytes"));
        ZASSERT(f.project.store->commit(std::move(pending)));
        auto aux = f.project.store->begin_store_aux("pch", "k");
        ZASSERT(!vfs::write(aux.tmp_path, "idx-bytes"));
        ZASSERT(f.project.store->commit(std::move(aux)));

        auto dep_id = f.project.file_table.intern(Spelling::absolute(dep_path));
        auto& st = f.project.pch_cache["k"];
        st.blob = "k";
        st.path = "k.pch";
        st.deps.push_back(
            {.path_id = dep_id, .version = f.project.file_table.intern_version(dep_id, 7)});
        f.project.mark_artifacts_dirty();
        f.save();

        auto blob = f.project.database->read(index::BlobKind::ArtifactRecords, "artifacts");
        ZASSERT(bool(blob));
        artifacts = blob.buffer->getBuffer().str();
    }

    {
        // The premise: under the current format the entry loads.
        IndexerFixture f;
        setup(f);
        f.load();
        ZASSERT(f.project.pch_cache.size() == std::size_t(1));

        // Put back the blob as an older binary would have written it.
        auto current = std::format("\"pch_index_format\":{}", index::index_format_version);
        auto stale = std::format("\"pch_index_format\":{}", index::index_format_version - 1);
        auto pos = artifacts.find(current);
        ZASSERT(pos != std::string::npos);
        artifacts.replace(pos, current.size(), stale);
        index::BlobDatabase::Blob blob{index::BlobKind::ArtifactRecords, "artifacts", artifacts};
        ZASSERT(f.project.database->write(blob, {}).empty());
    }

    IndexerFixture f;
    setup(f);
    f.load();
    ZASSERT(f.project.pch_cache.empty());
}

ZEST_CASE(DeplessPcmDropped) {
    // A dep-less PCM entry is an unvalidatable snapshot (the key embeds
    // no content): trusted, it would blindly serve a stale PCM after an
    // offline edit. Load drops it so the module rebuilds once.
    TempDir tmp;
    tmp.touch("mod.cppm", "export module m;\n");
    auto src = tmp.path("mod.cppm");

    auto setup = [&](IndexerFixture& f) {
        open_store(tmp, f.project);
        f.project.store->register_namespace({.name = "pcm", .extension = ".pcm"});
    };

    {
        IndexerFixture f;
        setup(f);
        auto pending = f.project.store->begin_store("pcm", "k");
        ZASSERT(!vfs::write(pending.tmp_path, "pcm-bytes"));
        ZASSERT(f.project.store->commit(std::move(pending)));

        auto& st = f.project.pcm_builds[f.project.file_table.intern(Spelling::absolute(src))];
        st.path = "m.pcm";
        st.key = "k";
        st.blob = "k";
        f.project.mark_artifacts_dirty();
        f.save();

        // The premise: the dep-less entry was persisted, so the tail
        // assertion exercises the load-side drop, not a write-side skip.
        auto blob = f.project.database->read(index::BlobKind::ArtifactRecords, "artifacts");
        ZASSERT(bool(blob));
        ZASSERT(blob.buffer->getBuffer().contains("\"key\":\"k\""));
    }

    IndexerFixture f;
    setup(f);
    f.load();
    ZASSERT(f.project.pcm_builds.empty());
}

ZEST_CASE(HeaderModePersisted) {
    // The NeedsContext result rides the artifacts blob: a restart with
    // unchanged disk content adopts it instead of re-running the trial.
    TempDir tmp;
    tmp.touch("utils.h", "inline int f();\n");
    auto path = tmp.path("utils.h");

    {
        IndexerFixture f;
        open_store(tmp, f.project);
        auto id = f.project.file_table.intern(Spelling::absolute(path));
        auto disk = f.project.file_table.disk.current(id);
        ZASSERT(disk);
        f.commands.record_header_mode(id, HeaderMode::NeedsContext, disk->hash);
        f.project.mark_artifacts_dirty();
        f.save();
    }

    IndexerFixture f;
    open_store(tmp, f.project);
    f.load();
    auto id = f.project.file_table.intern(Spelling::absolute(path));
    ZASSERT(f.commands.header_mode(id) == HeaderMode::NeedsContext);
}

ZEST_CASE(ContextsBlobRoundTrip) {
    // The editor's choices and artifact hosts ride the contexts blob: a
    // restart attaching an editor gets them back, and the save that
    // committed them resolves the durability ticket taken before it.
    TempDir tmp;
    tmp.touch("host.cpp", "#include \"h.h\"\n");
    tmp.touch("h.h", "int x;\n");
    auto host_path = tmp.path("host.cpp");
    auto header_path = tmp.path("h.h");

    {
        IndexerFixture f;
        open_store(tmp, f.project);
        EditorContext editor{f.project, f.commands, f.index_store.contexts};
        f.load();
        editor.load();
        auto host = f.project.file_table.intern(Spelling::absolute(host_path));
        auto header = f.project.file_table.intern(Spelling::absolute(header_path));
        editor.selections[header] = Selection{host, 1, "applied", "base"};
        editor.mark_dirty();
        auto ticket = f.index_store.contexts.ticket;
        f.save();
        ZASSERT(!f.index_store.contexts.dirty);
        ZASSERT(f.index_store.contexts.committed_ticket == ticket);
    }

    IndexerFixture f;
    open_store(tmp, f.project);
    EditorContext editor{f.project, f.commands, f.index_store.contexts};
    f.load();
    editor.load();
    auto host = f.project.file_table.intern(Spelling::absolute(host_path));
    auto header = f.project.file_table.intern(Spelling::absolute(header_path));
    auto* saved = editor.selection(header);
    ZASSERT(saved != nullptr);
    ZASSERT(saved->host_path_id == host);
    ZASSERT(saved->occurrence == std::optional<std::uint32_t>(1));
    ZASSERT(saved->command_hash == "applied");
    ZASSERT(saved->base_hash == "base");
    ZASSERT(!f.index_store.contexts.dirty);
}

ZEST_CASE(UnownedContextsPassThrough) {
    // A process with no editor (the batch commands) never rewrites the
    // contexts blob, yet carries its bytes into a database replacing a
    // corrupt one — the user's choices must not die with the old one.
    TempDir tmp;
    std::string bytes =
        R"({"paths":["/a.h"],"contexts":[{"file":0,"host":4294967295,"occurrence":4294967295,"command_hash":"c","base_hash":"b"}],"artifacts":[]})";
    inject_blob(tmp, index::BlobKind::ContextChoices, "contexts", bytes);

    IndexerFixture f;
    open_store(tmp, f.project);
    f.load();
    f.project.mark_artifacts_dirty();
    f.save();
    auto kept = f.project.database->read(index::BlobKind::ContextChoices, "contexts");
    ZASSERT(bool(kept));
    ZASSERT(kept.buffer->getBuffer() == bytes);

    f.reopen_database();
    ZASSERT(f.project.database != nullptr);
    ZASSERT(!bool(f.project.database->read(index::BlobKind::ContextChoices, "contexts")));
    f.save();
    auto rewritten = f.project.database->read(index::BlobKind::ContextChoices, "contexts");
    ZASSERT(bool(rewritten));
    ZASSERT(rewritten.buffer->getBuffer() == bytes);
}

ZEST_CASE(NoContextsNoRewrite) {
    // Nothing loaded, nothing to carry: a replaced database gets no empty
    // contexts blob, and no save keeps retrying one.
    TempDir tmp;
    IndexerFixture f;
    open_store(tmp, f.project);
    f.load();
    f.reopen_database();
    f.save();
    ZASSERT(!bool(f.project.database->read(index::BlobKind::ContextChoices, "contexts")));
}

};  // ZEST_SUITE(IndexerLoad)

ZEST_SUITE(IndexerRequeue) {

ZEST_CASE(PreemptionKeepsBudget) {
    IndexerFixture f;
    auto id = f.project.file_table.intern(Spelling::absolute("/proj/a.cpp"));
    f.pump.enqueue(id, ReindexReason::ContentChanged);

    // A preemption under memory pressure requeues without spending the
    // crash budget, no matter how often it repeats.
    for(unsigned i = 0; i < 2 * IndexerFixture::budget; ++i) {
        ZASSERT(int(f.fail(id, PendingTable::Failure::Preempted)) ==
                int(IndexerFixture::Decision::Requeued));
    }
    ZASSERT(f.attempts(id) == 0u);
    ZASSERT(f.pump.pending_reason(id));
}

ZEST_CASE(OwnCrashGivesUp) {
    IndexerFixture f;
    auto id = f.project.file_table.intern(Spelling::absolute("/proj/poison.cpp"));
    f.pump.enqueue(id, ReindexReason::ContentChanged);

    // The run named by its dying worker would crash the next one too: the
    // file waits for its content to change.
    ZASSERT(int(f.fail(id, PendingTable::Failure::Crashed)) ==
            int(IndexerFixture::Decision::GaveUp));
    ZASSERT(!f.pump.pending_reason(id).has_value());
}

ZEST_CASE(LostSpendsBudget) {
    IndexerFixture f;
    auto id = f.project.file_table.intern(Spelling::absolute("/proj/poison.cpp"));
    f.pump.enqueue(id, ReindexReason::ContentChanged);

    for(unsigned i = 0; i < IndexerFixture::budget; ++i) {
        ZASSERT(int(f.fail(id, PendingTable::Failure::Lost)) ==
                int(IndexerFixture::Decision::Requeued));
    }
    ZASSERT(f.attempts(id) == IndexerFixture::budget);

    // A preemption still requeues a file whose budget is spent: dropping
    // it would erase the pending state and serve the stale shard as fresh.
    // Only the next lost run gives up.
    ZASSERT(int(f.fail(id, PendingTable::Failure::Preempted)) ==
            int(IndexerFixture::Decision::Requeued));
    ZASSERT(f.attempts(id) == IndexerFixture::budget);

    // Giving up clears the pending slot: nothing is left to requeue, and
    // the stale shard serves as fresh — the accepted cost of abandoning.
    ZASSERT(int(f.fail(id, PendingTable::Failure::Lost)) == int(IndexerFixture::Decision::GaveUp));
    ZASSERT(!f.pump.pending_reason(id).has_value());
    ZASSERT(int(f.fail(id, PendingTable::Failure::Lost)) == int(IndexerFixture::Decision::Dropped));
}

ZEST_CASE(StaleCrashKeepsBudget) {
    IndexerFixture f;
    auto id = f.project.file_table.intern(Spelling::absolute("/proj/edited.cpp"));
    f.pump.enqueue(id, ReindexReason::ContentChanged);
    auto stale = f.ticket(id);

    // The user fixes the file while the old bytes' dispatch is in flight:
    // the stale crash must not spend the fixed content's budget or touch
    // its pending slot.
    f.pump.enqueue(id, ReindexReason::ContentChanged);
    ZASSERT(int(f.fail_at(id, stale, PendingTable::Failure::Lost)) ==
            int(IndexerFixture::Decision::Superseded));
    ZASSERT(f.attempts(id) == 0u);
    ZASSERT(f.pump.pending_reason(id));
}

ZEST_CASE(DepsDowngradeKeepsDebt) {
    IndexerFixture f;
    auto id = f.project.file_table.intern(Spelling::absolute("/proj/c.cpp"));
    f.pump.enqueue(id, ReindexReason::ContentChanged);
    auto launch = f.ticket(id);

    // The content pass is dispatched; a deps-only cascade arrives mid-flight
    // and downgrades the pending reason, betting on that pass to cover the
    // edit. The pass fails — the requeue must restore the ContentChanged
    // debt or the stale shard stops being suppressed.
    f.consume(id);
    f.pump.enqueue(id, ReindexReason::DepsOnly);
    ZASSERT(int(*f.pump.pending_reason(id)) == int(ReindexReason::DepsOnly));

    ZASSERT(int(f.fail_at(id, launch, PendingTable::Failure::Lost)) ==
            int(IndexerFixture::Decision::Requeued));
    ZASSERT(int(*f.pump.pending_reason(id)) == int(ReindexReason::ContentChanged));
    ZASSERT(f.attempts(id) == 1u);
}

ZEST_CASE(GaveUpClearsDowngraded) {
    IndexerFixture f;
    auto id = f.project.file_table.intern(Spelling::absolute("/proj/d.cpp"));
    f.pump.enqueue(id, ReindexReason::ContentChanged);
    auto launch = f.ticket(id);
    f.set_attempts(id, IndexerFixture::budget);

    // A deps-only enqueue arrives mid-flight, then the content pass spends
    // its last life. The downgraded entry must not stay queued: its retry
    // is doomed, and the give-up already accepted the staleness.
    f.consume(id);
    f.pump.enqueue(id, ReindexReason::DepsOnly);
    ZASSERT(int(f.fail_at(id, launch, PendingTable::Failure::Lost)) ==
            int(IndexerFixture::Decision::GaveUp));
    ZASSERT(!f.pump.pending_reason(id).has_value());
}

ZEST_CASE(DroppedWithoutPending) {
    IndexerFixture f;
    auto id = f.project.file_table.intern(Spelling::absolute("/proj/gone.cpp"));
    ZASSERT(int(f.fail(id, PendingTable::Failure::Lost)) == int(IndexerFixture::Decision::Dropped));
}

ZEST_CASE(AttemptWaitPerTicket) {
    IndexerFixture f;
    auto id = f.project.file_table.intern(Spelling::absolute("/proj/waited.cpp"));
    f.pump.enqueue(id, ReindexReason::ContentChanged);
    auto launch = f.ticket(id);

    bool first_woke = false;
    auto first_body = [&]() -> kota::task<> {
        co_await f.pump.await_attempt(id);
        first_woke = true;
    };
    auto first = first_body();
    f.loop.schedule(first);
    f.loop.run();

    // A requeue arrives mid-flight: the entry stays pending under a fresh
    // ticket, and a new waiter binds to that newer attempt.
    f.consume(id);
    f.pump.enqueue(id, ReindexReason::DepsOnly);
    bool second_woke = false;
    auto second_body = [&]() -> kota::task<> {
        co_await f.pump.await_attempt(id);
        second_woke = true;
    };
    auto second = second_body();
    f.loop.schedule(second);
    f.loop.run();

    // The flight's completion covers the ticket its waiter observed, no
    // matter the requeue: await_attempt promises one attempt, and holding
    // the waiter through every follow-up would park a feature request for
    // as long as edits keep arriving.
    f.wake_waiters(id, launch);
    ZASSERT(first_woke);
    ZASSERT(!second_woke);

    f.wake_waiters(id, f.ticket(id));
    ZASSERT(second_woke);
}

ZEST_CASE(ContentChangeResetsBudget) {
    IndexerFixture f;
    auto id = f.project.file_table.intern(Spelling::absolute("/proj/fixed.cpp"));
    f.pump.enqueue(id, ReindexReason::ContentChanged);

    ZASSERT(int(f.fail(id, PendingTable::Failure::Lost)) ==
            int(IndexerFixture::Decision::Requeued));
    ZASSERT(int(f.fail(id, PendingTable::Failure::Lost)) ==
            int(IndexerFixture::Decision::Requeued));
    ZASSERT(f.attempts(id) == 2u);

    // The user fixes the file: new content starts a fresh poison budget.
    f.pump.enqueue(id, ReindexReason::ContentChanged);
    ZASSERT(f.attempts(id) == 0u);

    // A deps-only cascade is not new content and keeps the attempt count.
    ZASSERT(int(f.fail(id, PendingTable::Failure::Lost)) ==
            int(IndexerFixture::Decision::Requeued));
    f.pump.enqueue(id, ReindexReason::DepsOnly);
    ZASSERT(f.attempts(id) == 1u);
}

ZEST_CASE(RoundSnapshotBoundary) {
    IndexerFixture f;
    // Manual rounds: the tail schedule() must no-op so the boundary between
    // the two rounds stays observable.
    f.project.config.project.enable_indexing.value = false;

    auto a = f.project.file_table.intern(Spelling::absolute("/fake/a.cpp"));
    auto b = f.project.file_table.intern(Spelling::absolute("/fake/b.cpp"));
    auto c = f.project.file_table.intern(Spelling::absolute("/fake/c.cpp"));

    f.pump.enqueue(a, ReindexReason::ContentChanged);
    f.pump.enqueue(b, ReindexReason::ContentChanged);

    // Grow the queue from inside the round: the first Report enqueues a
    // third file, which must arrive past the round snapshot and wait for the
    // next round instead of being consumed by this one.
    bool grew = false;
    IndexPump::Progress first_end;
    auto conn = f.pump.on_progress_changed.connect([&] {
        auto& progress = f.pump.progress();
        if(progress.stage == IndexPump::Progress::Stage::Report && !grew) {
            grew = true;
            f.pump.enqueue(c, ReindexReason::ContentChanged);
        }
        if(progress.stage == IndexPump::Progress::Stage::End && first_end.total == 0) {
            first_end = progress;
        }
    });

    f.run_round();

    ZASSERT(grew);
    ZASSERT(first_end.total == 2u);
    ZASSERT(first_end.dispatched == 2u);
    ZASSERT(first_end.completed == 2u);
    ZASSERT(f.pump.pending_files() == 1u);

    f.run_round();

    ZASSERT(f.pump.pending_files() == 0u);
    ZASSERT(f.pump.failed().size() == 3u);
    ZASSERT(f.pump.is_idle());
}

ZEST_CASE(PauseResumesRound) {
    IndexerFixture f;
    f.project.config.project.enable_indexing.value = false;

    auto a = f.project.file_table.intern(Spelling::absolute("/fake/a.cpp"));
    auto b = f.project.file_table.intern(Spelling::absolute("/fake/b.cpp"));
    f.pump.enqueue(a, ReindexReason::ContentChanged);
    f.pump.enqueue(b, ReindexReason::ContentChanged);

    // Pause from inside the round (the first Report), resume from a
    // separately scheduled task: the feeder must park on the resume event
    // and drain the rest of the round afterwards.
    bool paused = false;
    auto conn = f.pump.on_progress_changed.connect([&] {
        if(f.pump.progress().stage == IndexPump::Progress::Stage::Report && !paused) {
            paused = true;
            f.pump.pause_indexing();
        }
    });

    auto resume_body = [&]() -> kota::task<> {
        co_await kota::yield();
        f.pump.resume_indexing();
    };
    auto round = f.round_task();
    auto resumer = resume_body();
    f.loop.schedule(round);
    f.loop.schedule(resumer);
    f.loop.run();

    ZASSERT(paused);
    ZASSERT(f.pump.pending_files() == 0u);
    ZASSERT(f.pump.failed().size() == 2u);
    ZASSERT(f.pump.is_idle());
}

};  // ZEST_SUITE(IndexerRequeue)

/// The store's neutral change reports and the pump taking them — the
/// contracts the Indexer split introduced: every index-changing source
/// reports debt and index changes, the save carries the pump's debt
/// snapshot both ways.
ZEST_SUITE(IndexReports) {

ZEST_CASE(MergeReportsIndexChanged) {
    IndexerFixture f;
    TempDir tmp;
    tmp.touch("main.cpp", "int value() { return 1; }\n");
    auto indexed = index_file(tmp, tmp.path("main.cpp"));
    ZASSERT(!indexed.data.empty());

    llvm::SmallVector<Fid> notified;
    auto conn = f.pump.on_index_changed.connect(
        [&](llvm::ArrayRef<Fid> ids) { notified.append(ids.begin(), ids.end()); });
    ZASSERT(f.merge(indexed.data.data(), indexed.data.size()));

    ZASSERT(llvm::is_contained(notified,
                               f.project.file_table.intern(Spelling::absolute(indexed.tu_path))));
}

ZEST_CASE(DropReportsServedIndex) {
    // drop_index deletes entries an index-served session may already have
    // consumed; the report must carry every affected file so the serving
    // side can refresh — dropped entries change answers exactly like merged
    // entries do, and no later merge or compile is owed to cover them.
    IndexerFixture f;
    TempDir tmp;
    tmp.touch("dep.h", "#pragma once\ninline int dep() { return 1; }\n");
    tmp.touch("main.cpp", "#include \"dep.h\"\nint use() { return dep(); }\n");
    auto indexed = index_file(tmp, tmp.path("main.cpp"));
    ZASSERT(!indexed.data.empty());
    ZASSERT(f.merge(indexed.data.data(), indexed.data.size()));

    auto tu_id = f.project.file_table.intern(Spelling::absolute(indexed.tu_path));
    auto header_id = f.project.file_table.intern(Spelling::absolute(tmp.path("dep.h")));

    llvm::SmallVector<Fid> notified;
    auto conn = f.pump.on_index_changed.connect(
        [&](llvm::ArrayRef<Fid> ids) { notified.append(ids.begin(), ids.end()); });
    auto report = f.index_store.drop_index(tu_id);
    ZASSERT(llvm::is_contained(report.index_changed(), tu_id));
    ZASSERT(llvm::is_contained(report.index_changed(), header_id));

    f.pump.apply_report(report);
    ZASSERT(llvm::is_contained(notified, header_id));
}

ZEST_CASE(RetireReportsIndexChanged) {
    // The save-side recovery source: a shard retired by the compaction
    // vanishes from memory, which changes index-served answers exactly
    // like a merge — the report must say so.
    IndexerFixture f;
    TempDir tmp;
    tmp.touch("dep.h", "#pragma once\ninline int dep() { return 1; }\n");
    tmp.touch("main.cpp", "#include \"dep.h\"\nint use() { return dep(); }\n");
    open_store(tmp, f.project);
    auto indexed = index_file(tmp, tmp.path("main.cpp"));
    ZASSERT(!indexed.data.empty());
    ZASSERT(f.merge(indexed.data.data(), indexed.data.size()));
    f.save();

    // The TU stops including the header: its contribution dies, and the
    // next save retires the header's shard entirely.
    tmp.touch("main.cpp", "int use() { return 0; }\n");
    auto second = index_file(tmp, tmp.path("main.cpp"));
    ZASSERT(!second.data.empty());
    ZASSERT(f.merge(second.data.data(), second.data.size()));

    auto header_id = f.project.file_table.intern(Spelling::absolute(tmp.path("dep.h")));
    ZASSERT(f.project.index.shards.contains(header_id));

    llvm::SmallVector<Fid> notified;
    auto conn = f.pump.on_index_changed.connect(
        [&](llvm::ArrayRef<Fid> ids) { notified.append(ids.begin(), ids.end()); });
    f.save();

    ZASSERT(!f.project.index.shards.contains(header_id));
    ZASSERT(llvm::is_contained(notified, header_id));
}

ZEST_CASE(FailedStandaloneInSnapshot) {
    // A standalone header whose index attempt failed terminally is
    // recorded nowhere but the pump's failed set; the save's debt
    // snapshot must persist it, or the repair debt dies with the process
    // and nothing ever retries the header.
    TempDir tmp;
    tmp.touch("dep.h", "#pragma once\ninline int dep() { return 1; }\n");
    tmp.touch("main.cpp", "int use() { return 0; }\n");
    auto src = tmp.path("main.cpp");
    auto header = tmp.path("dep.h");

    {
        IndexerFixture f;
        open_store(tmp, f.project);
        f.project.cdb.add_command(tmp.root, src, llvm::StringRef("clang++ -c main.cpp"));
        f.mark_failed(f.project.file_table.intern(Spelling::absolute(header)));
        // Something dirty so the save writes at all; the snapshot rides
        // the same batch.
        auto indexed = index_file(tmp, src);
        ZASSERT(!indexed.data.empty());
        ZASSERT(f.merge(indexed.data.data(), indexed.data.size()));
        f.save();
    }

    IndexerFixture f;
    open_store(tmp, f.project);
    f.project.cdb.add_command(tmp.root, src, llvm::StringRef("clang++ -c main.cpp"));
    f.load();
    ZASSERT(f.pump.pending_reason(f.project.file_table.intern(Spelling::absolute(header))) ==
            ReindexReason::ContentChanged);
}

ZEST_CASE(LateDebtShutdownRetry) {
    // Debt showing up after the save serialized its snapshot (write-time
    // corruption recovery) comes back with snapshot_stale set; the
    // shutdown's one metadata retry must commit it to the fresh database,
    // or a dropped standalone header's repair debt is lost for good.
    struct CorruptOnWrite final : index::BlobDatabase {
        bool poisoned = false;

        index::ReadBlob read(index::BlobKind, llvm::StringRef) override {
            return {};
        }

        bool contains(index::BlobKind, llvm::StringRef) override {
            return false;
        }

        llvm::SmallVector<std::size_t> write(llvm::ArrayRef<Blob> puts,
                                             llvm::ArrayRef<index::BlobKey>) override {
            poisoned = true;
            llvm::SmallVector<std::size_t> failed;
            for(std::size_t i = 0; i < puts.size(); i += 1) {
                failed.push_back(i);
            }
            return failed;
        }

        void for_each_key(index::BlobKind,
                          llvm::function_ref<void(llvm::StringRef)>) override {}

        std::expected<std::uint64_t, std::string> advance_read_snapshot() override {
            return 0;
        }

        void retire_old_snapshot() override {}

        std::expected<bool, std::string> grow() override {
            return false;
        }

        bool corrupted() const override {
            return poisoned;
        }

        void condemn() override {}
    };

    IndexerFixture f;
    TempDir tmp;
    tmp.touch("dep.h", "#pragma once\ninline int dep() { return 1; }\n");
    tmp.touch("main.cpp", "int use() { return 0; }\n");
    open_store(tmp, f.project);
    f.project.database = std::make_unique<CorruptOnWrite>();

    f.mark_failed(f.project.file_table.intern(Spelling::absolute(tmp.path("dep.h"))));
    auto indexed = index_file(tmp, tmp.path("main.cpp"));
    ZASSERT(!indexed.data.empty());
    ZASSERT(f.merge(indexed.data.data(), indexed.data.size()));

    IndexStore::Report report;
    auto body = [&]() -> kota::task<> {
        report = co_await f.index_store.save(f.pump.save_debt());
    };
    auto task = body();
    f.loop.schedule(task);
    f.loop.run();

    ZASSERT(report.owes_retry);
    f.pump.apply_report(report);

    // The retry persists into the freshly reopened database, snapshot
    // included.
    f.save();
    auto blob = f.project.database->read(index::BlobKind::CommandSnapshot, "cdb");
    ZASSERT(bool(blob));
    ZASSERT(llvm::StringRef(blob.buffer->getBuffer()).contains("dep.h"));
}

ZEST_CASE(BoostRearmsIdleTimer) {
    // s#9: a boost colliding with an already-armed idle timer must re-arm
    // it to fire now. Un-fixed, this test waits out the full idle window
    // below instead of finishing promptly.
    IndexerFixture f;
    f.project.config.project.enable_indexing.value = true;
    f.project.config.project.idle_timeout_ms.value = 60'000;

    auto id = f.project.file_table.intern(Spelling::absolute("/fake/a.cpp"));
    f.pump.enqueue(id, ReindexReason::ContentChanged);
    f.pump.schedule();
    f.pump.boost(id);
    f.loop.run();

    ZASSERT(f.pump.is_idle());
    ZASSERT(f.pump.failed().size() == 1u);
}

ZEST_CASE(BoostSparesRunningRound) {
    // A file boosted while the running round has it in flight is queued
    // past the round and leads the next one. Moved into the running round,
    // it would run there a second time and push the round's last file out.
    IndexerFixture f;
    f.project.config.project.enable_indexing.value = true;
    f.project.config.project.idle_timeout_ms.value = 0;

    llvm::SmallVector<Fid> files;
    for(auto name: {"/fake/a.cpp", "/fake/b.cpp", "/fake/c.cpp"}) {
        files.push_back(f.project.file_table.intern(Spelling::absolute(name)));
        f.pump.enqueue(files.back(), ReindexReason::DepsOnly);
    }

    // Asked for each dispatched deps-only slot: the boost arrives while the
    // second file is taken.
    std::vector<llvm::SmallVector<Fid>> rounds(1);
    f.pump.compiled_by_session = [&](Fid id) {
        if(rounds.size() == 1 && rounds.front().size() == 1) {
            f.pump.boost(id);
        }
        rounds.back().push_back(id);
        return false;
    };
    auto conn = f.pump.on_progress_changed.connect([&] {
        if(f.pump.progress().stage == IndexPump::Progress::Stage::End) {
            rounds.emplace_back();
        }
    });
    f.pump.schedule();
    f.loop.run();

    ZEXPECT(rounds.size() == 3u);
    ZEXPECT(rounds[0] == files);
    ZEXPECT(rounds[1] == llvm::SmallVector<Fid>{files[1]});
    ZEXPECT(f.pump.is_idle());
}

};  // ZEST_SUITE(IndexReports)

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

    llvm::SmallVector<std::size_t> write(llvm::ArrayRef<Blob> puts,
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

ZEST_SUITE(IndexerPersist) {

ZEST_CASE(GrownMapRetriesAtOnce) {
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
    ZASSERT(f.merge(indexed.data.data(), indexed.data.size()));

    auto task = f.pump.persist();
    f.loop.schedule(task);
    f.loop.run();

    ZASSERT(log.refused == 1u);
    ZASSERT(log.writes.size() == 2u);
    ZASSERT(!f.index_store.has_unsaved_state());
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
    auto report = stack.store.merge(indexed.data.data(), indexed.data.size(), {});
    ZASSERT(report);
    stack.pump.apply_report(*report);

    auto task = stack.shutdown();
    loop.schedule(task);
    loop.run();

    // The shards commit without the search index a cold round never built.
    ZASSERT(log.writes.size() == 1u);
    ZASSERT(llvm::is_contained(log.writes[0], index::BlobKind::TUManifest));
    ZASSERT(!llvm::is_contained(log.writes[0], index::BlobKind::SearchIndex));
    ZASSERT(!project.index.search_index.loaded());
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
        ZASSERT(f.merge(indexed.data.data(), indexed.data.size()));
        auto box_id = f.project.file_table.intern(Spelling::absolute(tmp.path("box.h")));
        auto resolutions =
            f.project.index.instantiations.find(box_id)->second.resolutions();
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
        ZASSERT(f.merge(indexed.data.data(), indexed.data.size()));
        f.save();
        box_key = blob_key(f.project.file_table.resolve(
            f.project.file_table.intern(Spelling::absolute(tmp.path("box.h")))));
    }
    inject_blob(tmp,
                index::BlobKind::InstantiationShard,
                box_key,
                "corrupted beyond verification");

    IndexerFixture f;
    open_store(tmp, f.project);
    f.load();
    ZEXPECT(
        f.pump.pending_reason(f.project.file_table.intern(Spelling::absolute(tmp.path("a.cpp")))));
}

};  // ZEST_SUITE(IndexerPersist)

ZEST_SUITE(TURunLint) {

ZEST_CASE(ModuleLintScanParity) {
    // Only a scan under the lint round's extra args discovers the import
    // they gate, and edges it so the PCM exists when the worker's parse
    // (which sees the extras) consumes it. Only n.cppm runs, so the
    // import's PCM cannot arrive any other way.
    TempDir tmp;
    tmp.touch("m.cppm", "export module m;\nexport int mv() { return 1; }\n");
    tmp.touch("n.cppm",
              "export module n;\n"
              "#ifdef USE_M\n"
              "import m;\n"
              "export double half(int a, int b) { return a / b; }\n"
              "#endif\n");

    IndexerFixture f;
    write_cdb(tmp,
              f.project.cdb,
              build_cdb_json({
                  {tmp.root, tmp.path("m.cppm"), {}},
                  {tmp.root, tmp.path("n.cppm"), {}},
    }));
    scan_all(f.project.cdb, f.project.dep_graph);
    f.project.dep_graph.build_reverse_map();

    auto store = CacheStore::open(tmp.path("root"), 1);
    ZASSERT(store);
    store->register_namespace({.name = "pcm", .extension = ".pcm", .max_bytes = 1ull << 30});
    f.project.store.emplace(std::move(*store));

    f.pcm.register_runner();

    TURunFamily::Plan plan;
    plan.tidy = tidy::TidyParams{
        .checks = "-*,bugprone-integer-division",
        .extra_args = {"-DUSE_M"},
    };

    auto n_id = f.project.file_table.intern(Spelling::absolute(tmp.path("n.cppm")));
    TURunFamily::Outcome outcome;
    bool done = false;
    auto body = [&]() -> kota::task<> {
        WorkerPoolOptions opts;
        opts.self_path = clice_binary();
        opts.stateless_count = 1;
        opts.stateful_count = 0;
        ZASSERT(f.pool.start(opts));

        outcome = co_await f.turun.run(n_id, std::move(plan), {});

        co_await f.graph.shutdown();
        co_await f.pool.stop();
        done = true;
    };
    auto task = body();
    f.loop.schedule(task);
    f.loop.run();
    ZEXPECT(done);

    ZEXPECT(outcome.result == TURunFamily::Result::Completed);
    // The finding inside the gated region proves the parse saw the
    // extras and consumed the edge-built PCM.
    ZASSERT(!outcome.tidy_diagnostics.empty());
    ZEXPECT(outcome.tidy_diagnostics[0].check == "bugprone-integer-division");
}

};  // ZEST_SUITE(TURunLint)

ZEST_SUITE(TURunIndex) {

ZEST_CASE(OutdatedRerunsInFull) {
    // Two headers with the same bytes share one variant identity. Only
    // x1.h stores it, so b.cpp's x2.h section matches a known hash while
    // its own file stores nothing: that result must not commit, and the
    // rerun sends every section.
    TempDir tmp;
    tmp.touch("x1.h", "#pragma once\nint f();\n");
    tmp.touch("x2.h", "#pragma once\nint f();\n");
    tmp.touch("a.cpp", "#include \"x1.h\"\nint a() { return f(); }\n");
    tmp.touch("b.cpp", "#include \"x1.h\"\n#include \"x2.h\"\nint b() { return f(); }\n");

    IndexerFixture f;
    write_cdb(tmp,
              f.project.cdb,
              build_cdb_json({
                  {tmp.root, tmp.path("a.cpp"), {}},
                  {tmp.root, tmp.path("b.cpp"), {}},
    }));
    scan_all(f.project.cdb, f.project.dep_graph);
    f.project.dep_graph.build_reverse_map();

    auto a = index_file(tmp, tmp.path("a.cpp"));
    ZASSERT(f.merge(a.data.data(), a.data.size()));
    auto x2_id = f.project.file_table.intern(Spelling::absolute(tmp.path("x2.h")));
    ZASSERT(!f.project.index.shards.contains(x2_id));

    auto b_id = f.project.file_table.intern(Spelling::absolute(tmp.path("b.cpp")));
    std::vector<TURunFamily::Result> results;
    bool done = false;
    auto body = [&]() -> kota::task<> {
        WorkerPoolOptions opts;
        opts.self_path = clice_binary();
        opts.stateless_count = 1;
        opts.stateful_count = 0;
        ZASSERT(f.pool.start(opts));

        for(int run = 0; run < 2; run += 1) {
            TURunFamily::Plan plan;
            plan.index = true;
            results.push_back((co_await f.turun.run(b_id, std::move(plan), {})).result);
        }

        co_await f.graph.shutdown();
        co_await f.pool.stop();
        done = true;
    };
    auto task = body();
    f.loop.schedule(task);
    f.loop.run();
    ZEXPECT(done);

    using enum TURunFamily::Result;
    ZEXPECT(results == (std::vector<TURunFamily::Result>{Preempted, Completed}));
    ZEXPECT(f.project.index.shards.contains(x2_id));
}

ZEST_CASE(BuildsOnlyImportedPCMs) {
    // Indexing a module unit needs the PCMs of its imports, never its own:
    // the interface and an implementation partition nothing imports build
    // none, the implementation unit builds the interface it imports.
    TempDir tmp;
    tmp.touch("m.cppm", "export module m;\nexport int mv() { return 1; }\n");
    tmp.touch("detail.cpp", "module m:detail;\nint dv() { return 2; }\n");
    tmp.touch("impl.cpp", "module m;\nint uv() { return mv(); }\n");

    IndexerFixture f;
    write_cdb(tmp,
              f.project.cdb,
              build_cdb_json({
                  {tmp.root, tmp.path("m.cppm"),     {}},
                  {tmp.root, tmp.path("detail.cpp"), {}},
                  {tmp.root, tmp.path("impl.cpp"),   {}},
    }));
    scan_all(f.project.cdb, f.project.dep_graph);
    f.project.dep_graph.build_reverse_map();

    auto store = CacheStore::open(tmp.path("root"), 1);
    ZASSERT(store);
    store->register_namespace({.name = "pcm", .extension = ".pcm", .max_bytes = 1ull << 30});
    f.project.store.emplace(std::move(*store));
    f.pcm.register_runner();

    auto id = [&](llvm::StringRef name) {
        return f.project.file_table.intern(Spelling::absolute(tmp.path(name)));
    };
    std::vector<TURunFamily::Result> results;
    std::size_t pcms_before_impl = 0;
    bool done = false;
    auto body = [&]() -> kota::task<> {
        WorkerPoolOptions opts;
        opts.self_path = clice_binary();
        opts.stateless_count = 1;
        opts.stateful_count = 0;
        ZASSERT(f.pool.start(opts));

        for(auto name: {"m.cppm", "detail.cpp", "impl.cpp"}) {
            if(llvm::StringRef(name) == "impl.cpp") {
                pcms_before_impl = f.project.pcm_builds.size();
            }
            TURunFamily::Plan plan;
            plan.index = true;
            results.push_back((co_await f.turun.run(id(name), std::move(plan), {})).result);
        }

        co_await f.graph.shutdown();
        co_await f.pool.stop();
        done = true;
    };
    auto task = body();
    f.loop.schedule(task);
    f.loop.run();
    ZEXPECT(done);

    using enum TURunFamily::Result;
    ZEXPECT(results == (std::vector<TURunFamily::Result>{Completed, Completed, Completed}));
    ZEXPECT(pcms_before_impl == 0u);
    ZEXPECT(f.project.pcm_builds.size() == 1u);
    auto build = f.project.pcm_builds.find(id("m.cppm"));
    ZASSERT(build != f.project.pcm_builds.end());
    ZEXPECT(!build->second.path.empty());
}

};  // ZEST_SUITE(TURunIndex)

ZEST_SUITE(IndexerInstantiations) {

// box.h's template instantiated by units each with their own argument.
TempDir tmp;
IndexerFixture f;

IndexerInstantiationsTEST() {
    tmp.touch("box.h", R"(#pragma once
template <typename T>
struct Box {
    T value;
    void show() { value.print(); }
};
)");
    tmp.touch("a.h", R"(#pragma once
struct A { void print(); };
)");
    tmp.touch("b.h", R"(#pragma once
struct B { void print(); };
)");
    tmp.touch("a.cpp", R"(#include "box.h"
#include "a.h"
void a() { Box<A>{}.show(); }
)");
    tmp.touch("a2.cpp", R"(#include "box.h"
#include "a.h"
void a2() { Box<A>{}.show(); }
)");
    tmp.touch("b.cpp", R"(#include "box.h"
#include "b.h"
void b() { Box<B>{}.show(); }
)");
    write_cdb(tmp,
              f.project.cdb,
              build_cdb_json({
                  {tmp.root, tmp.path("a.cpp"),  {}},
                  {tmp.root, tmp.path("a2.cpp"), {}},
                  {tmp.root, tmp.path("b.cpp"),  {}},
    }));
    scan_all(f.project.cdb, f.project.dep_graph);
    f.project.dep_graph.build_reverse_map();
    open_store(tmp, f.project);
}

Fid id(llvm::StringRef name) {
    return f.project.file_table.intern(Spelling::absolute(tmp.path(name)));
}

/// Index a unit, skipping what the store offers it, and merge it.
bool index(llvm::StringRef name) {
    f.begin_round();
    auto known = f.index_store.known_instantiations(id(name));
    auto indexed = index_file(tmp, tmp.path(name), {}, {}, known);
    return !indexed.data.empty() && f.merge(indexed.data.data(), indexed.data.size());
}

const index::InstantiationShard* box() {
    return f.project.index.instantiation_shard(id("box.h"));
}

/// The live records naming `print` in box.h: one per argument some unit
/// instantiates the template with.
std::size_t prints() {
    std::size_t count = 0;
    box()->for_each_record([&](const index::InstantiationRecord& record) {
        count += record.kind == index::InstantiationKind::Reference ? 1 : 0;
        return true;
    });
    return count;
}

ZEST_CASE(ResolutionsSharedAndSkipped) {
    ZASSERT(index("a.cpp"));
    ZASSERT(box());
    ZEXPECT(box()->resolutions().size() == 1u);
    ZEXPECT(prints() == 1u);

    // a2.cpp skips Box<A>::show, which a.cpp walked, and uses its resolution.
    f.begin_round();
    auto known = f.index_store.known_instantiations(id("a2.cpp"));
    ZEXPECT(!known.empty());
    ZASSERT(index("a2.cpp"));
    auto& manifest = f.project.index.manifests.find(id("a2.cpp"))->second;
    ZASSERT(manifest.instantiations.size() == 1u);
    ZEXPECT(llvm::ArrayRef(box()->resolutions_of(manifest.instantiations.front().use)) ==
            box()->resolutions());

    ZASSERT(index("b.cpp"));
    ZEXPECT(box()->resolutions().size() == 2u);
    ZEXPECT(prints() == 2u);
}

ZEST_CASE(ResolutionLivesWhileUsed) {
    ZASSERT(index("a.cpp"));
    ZASSERT(index("a2.cpp"));
    ZASSERT(index("b.cpp"));

    // a2.cpp still uses Box<A>'s resolution when its producer leaves.
    f.drop_index(id("a.cpp"));
    ZEXPECT(prints() == 2u);
    // Without a current run behind it the key is offered no more.
    f.begin_round();
    auto key = f.index_store.known_instantiations(id("b.cpp"));
    ZEXPECT(key.empty());

    f.drop_index(id("a2.cpp"));
    ZEXPECT(prints() == 1u);
    f.save();
    ZEXPECT(box()->resolutions().size() == 1u);
}

ZEST_CASE(SkippedKeysFormNewUse) {
    // A unit skipping every key it meets walks nothing new, yet the
    // resolutions it uses together form a set no unit used before: the
    // file's blob must store it, or the manifest names a use nothing holds.
    tmp.touch("a2.cpp", R"(#include "box.h"
#include "a.h"
#include "b.h"
void a2() { Box<A>{}.show(); Box<B>{}.show(); }
)");
    ZASSERT(index("a.cpp"));
    ZASSERT(index("b.cpp"));
    ZASSERT(index("a2.cpp"));
    auto uses = f.project.index.manifests.find(id("a2.cpp"))->second.instantiations;
    ZASSERT(uses.size() == 1u);
    ZEXPECT(box()->has_use(uses.front().use));
    ZEXPECT(box()->resolutions_of(uses.front().use).size() == 2u);

    // a2.cpp alone keeps both resolutions alive.
    f.drop_index(id("a.cpp"));
    f.drop_index(id("b.cpp"));
    f.save();
    ZEXPECT(prints() == 2u);
}

ZEST_CASE(StaleProducerNotOffered) {
    ZASSERT(index("a.cpp"));
    f.begin_round();
    ZEXPECT(!f.index_store.known_instantiations(id("a2.cpp")).empty());
    // Its own run would confirm the key, not skip it.
    ZEXPECT(f.index_store.known_instantiations(id("a.cpp")).empty());
    // An open box.h stands in for its stored instantiations: it walks them.
    ZEXPECT(f.index_store.known_instantiations(id("box.h")).empty());

    // An edit of what the instantiation read makes its producer stale.
    tmp.touch("a.h", R"(#pragma once
struct A { void print(); void print(int); };
)");
    f.begin_round();
    ZEXPECT(f.index_store.known_instantiations(id("a2.cpp")).empty());
}

ZEST_CASE(ReplacedRunNotOffered) {
    ZASSERT(index("a.cpp"));
    // a.cpp's next run instantiates Box<A> no more: the key's run is gone.
    tmp.touch("a.cpp", R"(#include "box.h"
#include "a.h"
void a() {}
)");
    ZASSERT(index("a.cpp"));
    f.begin_round();
    ZEXPECT(f.index_store.known_instantiations(id("a2.cpp")).empty());
}

ZEST_CASE(GoneRunKeysCompacted) {
    // Box<A>::show resolves nothing: box.h holds its key alone.
    tmp.touch("box.h", R"(#pragma once
template <typename T>
struct Box {
    void show() {}
};
)");
    ZASSERT(index("a.cpp"));
    ZASSERT(box());
    ZEXPECT(box()->resolutions().empty());
    ZEXPECT(box()->keys().size() == 1u);

    // a.cpp's next run instantiates Box<A> no more: nothing of box.h's
    // instantiation shard holds, and the save retires it.
    tmp.touch("a.cpp", R"(#include "box.h"
#include "a.h"
void a() {}
)");
    ZASSERT(index("a.cpp"));
    f.save();
    ZEXPECT(!box());
}

ZEST_CASE(HeaderInternalUnkeyed) {
    // A type of a.h's anonymous namespace: what Box<A>::show resolves names
    // it outside box.h, which only a unit walking the instantiation lists
    // among the files naming `print`. No key is offered for it, so when
    // a.cpp stops instantiating, a2.cpp still leads from a.h to the record.
    tmp.touch("a.h", R"(#pragma once
namespace {
struct A { void print(); };
}
)");
    ZASSERT(index("a.cpp"));
    f.begin_round();
    ZEXPECT(f.index_store.known_instantiations(id("a2.cpp")).empty());
    ZASSERT(index("a2.cpp"));
    tmp.touch("a.cpp", R"(#include "box.h"
#include "a.h"
void a() {}
)");
    ZASSERT(index("a.cpp"));

    auto* header = f.project.index.shard(id("a.h"));
    ZASSERT(header);
    std::optional<index::SymbolHash> print;
    header->for_each_local_symbol(
        [&](index::SymbolHash hash, const index::SymbolView& identity) {
            if(identity.name == "print") {
                print = hash;
            }
            return true;
        });
    ZASSERT(print);
    llvm::SmallVector<Fid> reached;
    f.project.index.for_each_file_of_local(*print, id("a.h"), [&](Fid file) {
        reached.push_back(file);
    });
    ZEXPECT(llvm::is_contained(reached, id("box.h")));
}

ZEST_CASE(GoneKeyReruns) {
    ZASSERT(index("a.cpp"));
    f.begin_round();
    auto known = f.index_store.known_instantiations(id("a2.cpp"));
    ZASSERT(!known.empty());
    // The key leaves before a2.cpp's run that skipped it merges.
    f.drop_index(id("a.cpp"));
    f.save();
    auto indexed = index_file(tmp, tmp.path("a2.cpp"), {}, {}, known);
    auto merged = f.index_store.merge(indexed.data.data(), indexed.data.size(), {});
    ZASSERT(!merged);
    ZEXPECT(merged.error() == IndexStore::MergeError::Outdated);
}

};  // ZEST_SUITE(IndexerInstantiations)

}  // namespace
}  // namespace clice::testing

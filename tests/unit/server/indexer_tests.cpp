#include <expected>
#include <format>
#include <limits>
#include <memory>

#include "test/cdb_helper.h"
#include "test/temp_dir.h"
#include "test/test.h"
#include "command/argument_parser.h"
#include "compile/compilation.h"
#include "config/config.h"
#include "index/database.h"
#include "index/manifest.h"
#include "index/serialization.h"
#include "index/shard.h"
#include "index/tu_index.h"
#include "sched/context.h"
#include "sched/families/pcm.h"
#include "sched/families/turun.h"
#include "sched/graph.h"
#include "sched/index/pump.h"
#include "sched/index/store.h"
#include "sched/workspace.h"
#include "server/worker_test_helpers.h"
#include "support/cache_store.h"
#include "syntax/dependency_graph.h"
#include "worker/pool.h"

#include "kota/ipc/lsp/text.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/xxhash.h"

namespace clice::testing {

/// Test fixture with friend access to the pump's and the store's
/// internals. The claim wrappers (merge/save/load/drop_index) route each
/// store report back into the pump, as the production callers do.
struct IndexerFixture {
    using Verdict = PendingLedger::FailureVerdict;

    constexpr static unsigned budget = IndexPump::max_requeue_attempts;

    kota::event_loop loop;
    Workspace workspace;
    WorkerPool pool{loop};
    ContextResolver contexts{workspace};
    TaskGraph graph{loop};
    PCMFamily pcm{graph, workspace, contexts, pool};
    IndexStore index_store{loop, workspace};
    TURunFamily turun{graph, workspace, contexts, pcm, index_store, pool};
    IndexPump pump{loop, workspace, turun, index_store, pool};

    IndexerFixture() {
        turun.register_runner();
    }

    /// Merge a worker result and claim its report, as the TURun round does.
    bool merge(const void* data, std::size_t size) {
        auto report = index_store.merge(data, size);
        if(report) {
            pump.claim_report(*report);
        }
        return report.has_value();
    }

    /// Persist with the pump's debt snapshot and claim the report back, as
    /// the round tail does.
    kota::task<> async_save() {
        pump.claim_report(co_await index_store.save(pump.save_debt()));
    }

    /// Load and claim the report, as the workspace load does. Returns the
    /// decode verdict (false = old-format or corrupt global).
    bool load(bool read_only = false) {
        auto result = index_store.load(read_only);
        pump.claim_report(result.report);
        return result.decoded;
    }

    void drop_index(std::uint32_t id) {
        pump.claim_report(index_store.drop_index(id));
    }

    /// Record a terminally failed attempt, as run_index_task's failure
    /// verdicts do.
    void mark_failed(std::uint32_t id) {
        pump.failed_ids.insert(id);
    }

    /// Fail the entry's current dispatch: the launch ticket matches.
    Verdict fail(std::uint32_t id, bool crashed) {
        return pump.note_dispatch_failure({id, ticket(id)}, crashed);
    }

    /// Fail a dispatch launched with an explicit (possibly stale) ticket.
    Verdict fail_at(std::uint32_t id, std::uint64_t ticket, bool crashed) {
        return pump.note_dispatch_failure({id, ticket}, crashed);
    }

    std::uint64_t ticket(std::uint32_t id) {
        auto it = pump.ledger.entries.find(id);
        return it == pump.ledger.entries.end() ? std::numeric_limits<std::uint64_t>::max()
                                               : it->second.ticket;
    }

    unsigned attempts(std::uint32_t id) {
        auto it = pump.ledger.entries.find(id);
        return it == pump.ledger.entries.end() ? 0u : it->second.requeue_attempts;
    }

    void set_attempts(std::uint32_t id, unsigned n) {
        pump.ledger.entries.find(id)->second.requeue_attempts = n;
    }

    /// Consume the queued slot as a dispatch would, so a later enqueue
    /// takes the fresh-slot (mid-flight) path.
    void consume(std::uint32_t id) {
        pump.ledger.queued.erase(id);
    }

    /// Complete an attempt for `ticket` on the loop, as run_index_task's
    /// tail does — waking waiters requires a running loop.
    void settle(std::uint32_t id, std::uint64_t ticket) {
        auto body = [&]() -> kota::task<> {
            pump.settle_attempt_waits(id, ticket);
            co_return;
        };
        auto task = body();
        loop.schedule(task);
        loop.run();
    }

    /// Start a fresh staleness round: per-round FileVersion verdicts are
    /// cleared by run_background_indexing, which these tests bypass.
    void clear_verdicts() {
        index_store.begin_round();
    }

    /// Judge staleness inside the current round (see clear_verdicts).
    bool need_update(llvm::StringRef path) {
        return index_store.need_update(path);
    }

    bool global_dirty() {
        return index_store.global_dirty;
    }

    /// Drop the merge's own dirty mark so a later assertion isolates the
    /// stamp-repair path.
    void reset_global_dirty() {
        index_store.global_dirty = false;
    }

    /// Record a standalone header's borrowed host, as the TURun round does
    /// after its merge lands (these tests merge worker results directly).
    void set_header_host(std::uint32_t header_id, std::uint32_t host_id) {
        index_store.record_header_host(header_id, host_id);
    }

    /// Run one save() to completion on the fixture's loop.
    void save() {
        auto task = async_save();
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
    std::string data;     ///< Envelope bytes, as a worker would ship them.
    std::string tu_path;  ///< The TU's canonical path inside the index.
};

/// Index a real on-disk file in-process into its envelope bytes.
IndexedTU index_file(TempDir& tmp, llvm::StringRef file, std::vector<std::string> extra_args = {}) {
    std::string resource = std::string(resource_dir());
    std::vector<std::string> args =
        {"clang++", "-fsyntax-only", "-resource-dir", resource, "-c", std::string(file)};
    args.insert(args.end(), extra_args.begin(), extra_args.end());

    CompilationParams cp;
    cp.kind = CompilationKind::Indexing;
    cp.directory = std::string(tmp.root);
    for(auto& arg: args) {
        cp.arguments.push_back(arg.c_str());
    }

    auto unit = compile(cp);
    if(!unit.completed()) {
        return {};
    }
    IndexedTU result;
    result.data = index::build_tu_index(unit);
    auto view = index::TUIndex::from_bytes(result.data);
    if(!view.loaded()) {
        return {};
    }
    result.tu_path = std::string(view.path(view.path_count() - 1));
    return result;
}

/// Re-encode an envelope with its consumed-content hash column dropped,
/// as a file behind a PCM ships it. Field order MUST mirror the envelope
/// layout (tu_index.cpp).
std::string strip_path_hashes(llvm::StringRef data) {
    struct SymbolMirror {
        std::string name;
        std::uint8_t kind = 0;
        std::uint8_t scope = 0;
        std::vector<std::byte> reference_files;
    };

    struct SectionMirror {
        std::uint32_t path_id = 0;
        std::uint64_t hash = 0;
        std::vector<std::uint8_t> blob;
    };

    struct EnvelopeMirror {
        std::uint32_t format_version = index::index_format_version;
        std::int64_t built_at = 0;
        std::vector<std::string> paths;
        std::vector<std::uint64_t> path_hashes;
        std::vector<index::IncludeLocation> locations;
        llvm::DenseMap<std::uint64_t, SymbolMirror> symbols{};
        std::vector<SectionMirror> sections;
    };

    auto view = index::TUIndex::from_bytes(data);
    EnvelopeMirror mirror;
    mirror.built_at = view.built_at();
    for(std::uint32_t i = 0; i < view.path_count(); i += 1) {
        mirror.paths.emplace_back(view.path(i));
    }
    for(std::uint32_t i = 0; i < view.location_count(); i += 1) {
        mirror.locations.push_back(view.location(i));
    }
    view.iterate_symbols(
        [&](index::SymbolHash hash, const index::SymbolIdentity& id, llvm::StringRef bitmap) {
            auto& symbol = mirror.symbols[hash];
            symbol.name = std::string(id.name);
            symbol.kind = id.kind.value();
            symbol.scope = static_cast<std::uint8_t>(id.scope);
            const auto* begin = reinterpret_cast<const std::byte*>(bitmap.data());
            symbol.reference_files.assign(begin, begin + bitmap.size());
            return true;
        });
    for(std::uint32_t i = 0; i < view.section_count(); i += 1) {
        auto blob = view.section_blob(i);
        mirror.sections.push_back({view.section_path(i),
                                   view.section_hash(i),
                                   std::vector<std::uint8_t>(blob.begin(), blob.end())});
    }

    auto bytes = kota::codec::fbs::to_bytes(mirror);
    if(!bytes) {
        return {};
    }
    return std::string(bytes->begin(), bytes->end());
}

/// A structurally valid blob of `text`'s content generation carrying an
/// explicit variant list — the shapes load()'s healing paths probe.
std::string planted_blob(llvm::StringRef text, std::uint64_t variant) {
    index::ShardBlob blob;
    blob.format_version = index::index_format_version;
    blob.content_hash = llvm::xxh3_64bits(text);
    blob.content_size = static_cast<std::uint32_t>(text.size());
    auto starts = kota::ipc::lsp::build_line_starts(std::string_view(text.data(), text.size()));
    for(std::size_t i = 0; i < starts.size(); i += 1) {
        auto next = i + 1 < starts.size() ? starts[i + 1] : blob.content_size;
        blob.line_lengths.push_back(static_cast<std::uint8_t>(next - starts[i]));
    }
    blob.variants = {variant};
    blob.sym_hashes = {111};
    blob.sym_rel_offsets = {0, 0};
    std::string bytes;
    llvm::raw_string_ostream os(bytes);
    index::serialize_blob(blob, os);
    return bytes;
}

void open_store(TempDir& tmp, Workspace& workspace) {
    auto store = CacheStore::open(tmp.path("cache"), 1);
    ASSERT_TRUE(store.has_value());
    workspace.store.emplace(std::move(*store));
    workspace.index_db = index::open_fs_database(*workspace.store);
}

/// The storage key of a file's shard or manifest blob (the store's naming).
std::string blob_key(llvm::StringRef path) {
    return std::format("{:016x}", llvm::xxh3_64bits(path));
}

TEST_SUITE(IndexerMerge) {

IndexerFixture fx;
kota::event_loop& loop = fx.loop;
Workspace& workspace = fx.workspace;
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

void drop_index(std::uint32_t id) {
    fx.drop_index(id);
}

TEST_CASE(MergeRejectsGarbage) {
    // A worker shipping corrupted bytes (torn write, stale format) must not
    // crash the master or leave partial state behind.
    ASSERT_TRUE(workspace.shards.empty());
    ASSERT_TRUE(workspace.project_index.symbols.empty());

    std::string garbage = "definitely not a flatbuffer, but long enough to try";
    ASSERT_FALSE(merge(garbage.data(), garbage.size()));

    ASSERT_TRUE(workspace.shards.empty());
    ASSERT_TRUE(workspace.project_index.symbols.empty());
}

TEST_CASE(MergeIgnoresDiskDrift) {
    TempDir tmp;
    tmp.touch("main.cpp", "int value() { return 1; }\n");
    auto src = tmp.path("main.cpp");

    auto indexed = index_file(tmp, src);
    ASSERT_FALSE(indexed.data.empty());

    merge(indexed.data.data(), indexed.data.size());
    auto path_id = workspace.path_pool.intern(indexed.tu_path);
    auto it = workspace.shards.find(path_id);
    ASSERT_TRUE(it != workspace.shards.end());
    ASSERT_EQ(it->second.content_hash(), llvm::xxh3_64bits("int value() { return 1; }\n"));

    // The disk moved on since the rows were indexed. The blob is
    // self-contained — its rows pair with the generation it embeds, never
    // with the disk — so the re-merge is a pure variant hit and freshness
    // gating owns the drift.
    tmp.touch("main.cpp", "int renamed() { return 2; }\n");
    merge(indexed.data.data(), indexed.data.size());
    ASSERT_EQ(it->second.content_hash(), llvm::xxh3_64bits("int value() { return 1; }\n"));
    ASSERT_EQ(it->second.variants().size(), std::size_t(1));
    ASSERT_TRUE(workspace.project_index.contributions.lookup(path_id).contains(path_id));

    // Rows built from the settled content open a new generation.
    auto fresh = index_file(tmp, src);
    ASSERT_FALSE(fresh.data.empty());
    merge(fresh.data.data(), fresh.data.size());
    ASSERT_EQ(it->second.content_hash(), llvm::xxh3_64bits("int renamed() { return 2; }\n"));
}

TEST_CASE(SaveCommitsDirtyShard) {
    TempDir tmp;
    tmp.touch("main.cpp", "int flip_value() { return 1; }\n");
    auto src = tmp.path("main.cpp");
    open_store(tmp, workspace);

    auto indexed = index_file(tmp, src);
    ASSERT_FALSE(indexed.data.empty());
    merge(indexed.data.data(), indexed.data.size());

    auto path_id = workspace.path_pool.intern(indexed.tu_path);
    ASSERT_EQ(index_store.pending_shard_writes(), 1u);

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
    auto it = workspace.shards.find(path_id);
    ASSERT_TRUE(it != workspace.shards.end());
    ASSERT_EQ(index_store.pending_shard_writes(), 0u);
    ASSERT_EQ(index_store.last_save_shards(), 1u);
    ASSERT_EQ(it->second.content_hash(), llvm::xxh3_64bits("int flip_value() { return 1; }\n"));
    ASSERT_TRUE(workspace.project_index.contributions.lookup(path_id).contains(path_id));
}

TEST_CASE(SaveMigratesShardViews) {
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

        index::ReadBlob read(index::IndexBlobKind kind, llvm::StringRef key) override {
            return real->read(kind, key);
        }

        bool contains(index::IndexBlobKind kind, llvm::StringRef key) override {
            return real->contains(kind, key);
        }

        llvm::SmallVector<std::size_t> write(llvm::ArrayRef<Blob> puts,
                                             llvm::ArrayRef<index::BlobKey> removes) override {
            return real->write(puts, removes);
        }

        void for_each_key(index::IndexBlobKind kind,
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
    ASSERT_TRUE(store.has_value());
    workspace.store.emplace(std::move(*store));
    auto spy = std::make_unique<SnapshotSpy>();
    spy->real = index::open_lmdb_database(*workspace.store);
    ASSERT_TRUE(spy->real != nullptr);
    auto* probe = spy.get();
    workspace.index_db = std::move(spy);

    auto indexed = index_file(tmp, src);
    ASSERT_FALSE(indexed.data.empty());
    merge(indexed.data.data(), indexed.data.size());
    auto path_id = workspace.path_pool.intern(indexed.tu_path);
    auto before = workspace.shards.find(path_id);
    ASSERT_TRUE(before != workspace.shards.end());
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
    ASSERT_EQ(probe->advances, 1);
    ASSERT_EQ(probe->retires, 1);
    auto it = workspace.shards.find(path_id);
    ASSERT_TRUE(it != workspace.shards.end());
    ASSERT_TRUE(it->second.loaded());
    ASSERT_TRUE(it->second.bytes().data() != bytes_before);
    ASSERT_EQ(it->second.content_hash(), llvm::xxh3_64bits("int migrate_value() { return 1; }\n"));
    ASSERT_TRUE(it->second.variants() == variants_before);
}

TEST_CASE(GrowFailureShedsCleanShards) {
    TempDir tmp;
    tmp.touch("clean.cpp", "int clean_value() { return 1; }\n");
    tmp.touch("dirty.cpp", "int dirty_value() { return 2; }\n");
    open_store(tmp, workspace);

    // grow() failing is backend-independent shed territory: every clean
    // (non-dirty, possibly borrowed) shard must go with its owner requeued
    // — the manifests still read fresh, so nothing else would rebuild the
    // dropped rows — while dirty ones are owned by construction and stay.
    // The spy also fails dirty.cpp's put so it is re-dirtied by the time
    // the migration runs.
    struct FailingGrow final : index::BlobDatabase {
        std::unique_ptr<index::BlobDatabase> real;
        std::string fail_key;

        index::ReadBlob read(index::IndexBlobKind kind, llvm::StringRef key) override {
            return real->read(kind, key);
        }

        bool contains(index::IndexBlobKind kind, llvm::StringRef key) override {
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

        void for_each_key(index::IndexBlobKind kind,
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
    ASSERT_FALSE(indexed_clean.data.empty());
    ASSERT_FALSE(indexed_dirty.data.empty());

    auto spy = std::make_unique<FailingGrow>();
    spy->real = std::move(workspace.index_db);
    // Through the pool: the indexer keys blobs by the pool-canonical path,
    // which need not equal the raw temp path byte-for-byte (Windows 8.3
    // names).
    spy->fail_key =
        blob_key(workspace.path_pool.resolve(workspace.path_pool.intern(indexed_dirty.tu_path)));
    workspace.index_db = std::move(spy);

    merge(indexed_clean.data.data(), indexed_clean.data.size());
    merge(indexed_dirty.data.data(), indexed_dirty.data.size());
    auto clean_id = workspace.path_pool.intern(indexed_clean.tu_path);
    auto dirty_id = workspace.path_pool.intern(indexed_dirty.tu_path);

    auto save_body = [&]() -> kota::task<> {
        co_await async_save();
    };
    auto task = save_body();
    loop.schedule(task);
    loop.run();

    ASSERT_FALSE(workspace.shards.contains(clean_id));
    ASSERT_TRUE(workspace.shards.contains(dirty_id));
    ASSERT_TRUE(pump.pending_reason(clean_id) == ReindexReason::ContentChanged);
}

TEST_CASE(MidSaveMergeKept) {
    TempDir tmp;
    tmp.touch("main.cpp", "int first_value() { return 1; }\n");
    auto src = tmp.path("main.cpp");
    open_store(tmp, workspace);

    auto indexed = index_file(tmp, src);
    ASSERT_FALSE(indexed.data.empty());
    merge(indexed.data.data(), indexed.data.size());
    auto path_id = workspace.path_pool.intern(indexed.tu_path);

    // Prepared before save() starts so the interleaved merge is purely an
    // in-memory event.
    tmp.touch("main.cpp", "int second_value() { return 2; }\n");
    auto fresh = index_file(tmp, src);
    ASSERT_FALSE(fresh.data.empty());

    // The merge task runs when save() suspends at its write await: it
    // lands after the dirty snapshot was taken and cleared, exactly the
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

    // Sampled while save() awaited its commit: the settle gauge must keep
    // covering the in-flight batch, or a stats poll in that window reads
    // "settled" with last_save_shards still holding its reset.
    ASSERT_TRUE(mid_save_pending >= 1);

    // The save committed the pre-merge snapshot: the shard keeps the new
    // content and stays dirty so the next save commits it.
    auto it = workspace.shards.find(path_id);
    ASSERT_TRUE(it != workspace.shards.end());
    ASSERT_EQ(index_store.pending_shard_writes(), 1u);
    ASSERT_EQ(it->second.content_hash(), llvm::xxh3_64bits("int second_value() { return 2; }\n"));

    auto again_body = [&]() -> kota::task<> {
        co_await async_save();
    };
    auto task = again_body();
    loop.schedule(task);
    loop.run();

    it = workspace.shards.find(path_id);
    ASSERT_EQ(index_store.pending_shard_writes(), 0u);
    ASSERT_EQ(it->second.content_hash(), llvm::xxh3_64bits("int second_value() { return 2; }\n"));
}

TEST_CASE(MergeHitWritesNothing) {
    TempDir tmp;
    tmp.touch("main.cpp", "int steady() { return 1; }\n");
    auto src = tmp.path("main.cpp");
    open_store(tmp, workspace);

    auto indexed = index_file(tmp, src);
    ASSERT_FALSE(indexed.data.empty());
    merge(indexed.data.data(), indexed.data.size());
    auto save_body = [&]() -> kota::task<> {
        co_await async_save();
    };
    auto task = save_body();
    loop.schedule(task);
    loop.run();
    ASSERT_EQ(index_store.pending_shard_writes(), 0u);

    // A re-merge whose rows the shard already stores is the steady state of
    // every background round: it must record contributions and touch no
    // blob at all.
    merge(indexed.data.data(), indexed.data.size());
    ASSERT_EQ(index_store.pending_shard_writes(), 0u);
}

TEST_CASE(SharedHeaderVariants) {
    TempDir tmp;
    tmp.touch("shared.h",
              "#pragma once\n#ifdef MODE\nint mode_fn();\n#endif\n"
              "inline int shared_fn() { return 1; }\n");
    tmp.touch("a.cpp", "#include \"shared.h\"\nint a() { return shared_fn(); }\n");
    tmp.touch("b.cpp", "#include \"shared.h\"\nint b() { return shared_fn(); }\n");

    auto a = index_file(tmp, tmp.path("a.cpp"));
    auto b = index_file(tmp, tmp.path("b.cpp"), {"-DMODE"});
    ASSERT_FALSE(a.data.empty());
    ASSERT_FALSE(b.data.empty());

    // Two TUs preprocess the header differently: both variants coexist in
    // one blob, each TU's contribution live.
    merge(a.data.data(), a.data.size());
    merge(b.data.data(), b.data.size());
    auto header_id = workspace.path_pool.intern(tmp.path("shared.h"));
    auto& shard = workspace.shards[header_id];
    ASSERT_EQ(shard.variants().size(), std::size_t(2));
    ASSERT_EQ(workspace.project_index.contributions.lookup(header_id).size(), std::size_t(2));

    // A third TU sharing a's preprocessing hits the stored variant: the
    // set does not grow, and neither existing contribution is disturbed.
    tmp.touch("c.cpp", "#include \"shared.h\"\nint c() { return shared_fn(); }\n");
    auto c = index_file(tmp, tmp.path("c.cpp"));
    ASSERT_FALSE(c.data.empty());
    merge(c.data.data(), c.data.size());
    ASSERT_EQ(shard.variants().size(), std::size_t(2));
    ASSERT_EQ(workspace.project_index.contributions.lookup(header_id).size(), std::size_t(3));

    // Re-indexing a TU whose header rows are unchanged must not disturb
    // the other TUs' variants either.
    tmp.touch("a.cpp", "#include \"shared.h\"\nint a2() { return shared_fn(); }\n");
    auto fresh = index_file(tmp, tmp.path("a.cpp"));
    ASSERT_FALSE(fresh.data.empty());
    merge(fresh.data.data(), fresh.data.size());
    ASSERT_EQ(shard.variants().size(), std::size_t(2));
    ASSERT_EQ(workspace.project_index.contributions.lookup(header_id).size(), std::size_t(3));
}

TEST_CASE(HeaderRegenerationReplaces) {
    TempDir tmp;
    tmp.touch("dep.h", "#pragma once\ninline int dep() { return 1; }\n");
    tmp.touch("main.cpp", "#include \"dep.h\"\nint use() { return dep(); }\n");
    auto src = tmp.path("main.cpp");

    auto v1 = index_file(tmp, src);
    ASSERT_FALSE(v1.data.empty());
    merge(v1.data.data(), v1.data.size());
    auto header_id = workspace.path_pool.intern(tmp.path("dep.h"));
    auto tu_id = workspace.path_pool.intern(v1.tu_path);
    auto old_hash = workspace.project_index.contributions.lookup(header_id).lookup(tu_id);
    ASSERT_TRUE(old_hash != 0);

    // The header changes, a reindex captures it — and the header changes
    // AGAIN before the result merges. The worker's bytes are their own
    // generation: they land verbatim regardless of the disk moving on, and
    // freshness gating owns the remaining drift.
    tmp.touch("dep.h", "#pragma once\ninline int dep() { return 2; }\n");
    auto v2 = index_file(tmp, src);
    ASSERT_FALSE(v2.data.empty());
    tmp.touch("dep.h", "#pragma once\ninline int dep() { return 3; }\n");

    merge(v2.data.data(), v2.data.size());
    auto new_hash = workspace.project_index.contributions.lookup(header_id).lookup(tu_id);
    ASSERT_TRUE(new_hash != 0);
    ASSERT_TRUE(new_hash != old_hash);
    ASSERT_TRUE(workspace.shards[header_id].has_variant(new_hash));
    // A new content generation never shares row storage with the old one.
    ASSERT_FALSE(workspace.shards[header_id].has_variant(old_hash));
    ASSERT_EQ(workspace.shards[header_id].content_hash(),
              llvm::xxh3_64bits("#pragma once\ninline int dep() { return 2; }\n"));
}

TEST_CASE(SaveCompactsAndRetires) {
    TempDir tmp;
    tmp.touch("shared.h",
              "#pragma once\n#ifdef MODE\nint mode_fn();\n#endif\n"
              "inline int shared_fn() { return 1; }\n");
    tmp.touch("a.cpp", "#include \"shared.h\"\nint a() { return shared_fn(); }\n");
    tmp.touch("b.cpp", "#include \"shared.h\"\nint b() { return shared_fn(); }\n");
    open_store(tmp, workspace);

    auto a = index_file(tmp, tmp.path("a.cpp"));
    auto b = index_file(tmp, tmp.path("b.cpp"), {"-DMODE"});
    ASSERT_FALSE(a.data.empty());
    ASSERT_FALSE(b.data.empty());
    merge(a.data.data(), a.data.size());
    merge(b.data.data(), b.data.size());
    auto header_id = workspace.path_pool.intern(tmp.path("shared.h"));
    ASSERT_EQ(workspace.shards[header_id].variants().size(), std::size_t(2));

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
    // erases the dead rows for real.
    tmp.touch("b.cpp", "int b() { return 2; }\n");
    auto b2 = index_file(tmp, tmp.path("b.cpp"));
    ASSERT_FALSE(b2.data.empty());
    merge(b2.data.data(), b2.data.size());
    ASSERT_TRUE(workspace.shards[header_id].has_dead_variants());
    save();
    ASSERT_EQ(workspace.shards[header_id].variants().size(), std::size_t(1));

    // a drops it too: no contribution is left, so the shard retires from
    // memory and from storage — with no owner left to re-enqueue.
    tmp.touch("a.cpp", "int a() { return 3; }\n");
    auto a2 = index_file(tmp, tmp.path("a.cpp"));
    ASSERT_FALSE(a2.data.empty());
    merge(a2.data.data(), a2.data.size());
    save();
    ASSERT_FALSE(workspace.shards.contains(header_id));
    ASSERT_FALSE(pump.pending_reason(workspace.path_pool.intern(a2.tu_path)).has_value());
    bool on_disk = false;
    auto key = blob_key(workspace.path_pool.resolve(header_id));
    workspace.index_db->for_each_key(index::IndexBlobKind::Shard,
                                     [&](llvm::StringRef k) { on_disk |= k == key; });
    ASSERT_FALSE(on_disk);
}

TEST_CASE(SaveRetiresPinnedShard) {
    TempDir tmp;
    tmp.touch("pinned.h",
              "#pragma once\n#ifdef MODE\nint pin_mode();\n#endif\n"
              "inline int pin_fn() { return 1; }\n");
    tmp.touch("pa.cpp", "#include \"pinned.h\"\nint pa() { return pin_fn(); }\n");
    tmp.touch("pb.cpp", "#include \"pinned.h\"\nint pb() { return pin_fn(); }\n");
    open_store(tmp, workspace);

    auto a = index_file(tmp, tmp.path("pa.cpp"));
    auto b = index_file(tmp, tmp.path("pb.cpp"), {"-DMODE"});
    ASSERT_FALSE(a.data.empty());
    ASSERT_FALSE(b.data.empty());
    merge(a.data.data(), a.data.size());
    merge(b.data.data(), b.data.size());
    auto header_id = workspace.path_pool.intern(tmp.path("pinned.h"));
    ASSERT_EQ(workspace.shards[header_id].variants().size(), std::size_t(2));

    // The header moves to a new content generation and only pa catches up:
    // the blob starts over with pa's variant, while pb's manifest still
    // pins a hash the blob no longer stores.
    tmp.touch("pinned.h",
              "#pragma once\n#ifdef MODE\nint pin_mode();\n#endif\n"
              "inline int pin_fn() { return 2; }\n");
    auto a2 = index_file(tmp, tmp.path("pa.cpp"));
    ASSERT_FALSE(a2.data.empty());
    merge(a2.data.data(), a2.data.size());
    ASSERT_EQ(workspace.shards[header_id].variants().size(), std::size_t(1));

    // The rebuild re-enqueued pb; its pass then runs and fails, consuming
    // the slot — the state the retirement below must repair on its own.
    pump.clear_pending(workspace.path_pool.intern(b.tu_path));

    // pa's index drops before pb reindexes: every stored variant is dead,
    // but pb's pinned hash keeps the live set nonempty. The save must
    // retire the shard rather than compact to an empty variant set.
    drop_index(workspace.path_pool.intern(a2.tu_path));
    auto body = [&]() -> kota::task<> {
        co_await async_save();
    };
    auto task = body();
    loop.schedule(task);
    loop.run();

    ASSERT_FALSE(workspace.shards.contains(header_id));
    bool on_disk = false;
    auto key = blob_key(workspace.path_pool.resolve(header_id));
    workspace.index_db->for_each_key(index::IndexBlobKind::Shard,
                                     [&](llvm::StringRef k) { on_disk |= k == key; });
    ASSERT_FALSE(on_disk);

    // pb's manifest survives, still pinning rows the retirement made
    // unservable; nothing else in this process would rebuild them (a
    // reverted header even reads fresh by hash), so the retirement must
    // re-enqueue pb itself.
    ASSERT_TRUE(pump.pending_reason(workspace.path_pool.intern(b.tu_path)) ==
                ReindexReason::ContentChanged);
}

TEST_CASE(RebuildRequeuesPinnedOwner) {
    TempDir tmp;
    tmp.touch("gen.h",
              "#pragma once\n#ifdef MODE\nint gen_mode();\n#endif\n"
              "inline int gen_fn() { return 1; }\n");
    tmp.touch("ga.cpp", "#include \"gen.h\"\nint ga() { return gen_fn(); }\n");
    tmp.touch("gb.cpp", "#include \"gen.h\"\nint gb() { return gen_fn(); }\n");

    auto a = index_file(tmp, tmp.path("ga.cpp"));
    auto b = index_file(tmp, tmp.path("gb.cpp"), {"-DMODE"});
    ASSERT_FALSE(a.data.empty());
    ASSERT_FALSE(b.data.empty());
    merge(a.data.data(), a.data.size());
    merge(b.data.data(), b.data.size());
    auto b_tu = workspace.path_pool.intern(b.tu_path);
    ASSERT_FALSE(pump.pending_reason(b_tu).has_value());

    // The header moves to a new content generation and only ga catches up:
    // the rebuilt blob discards gb's variant. With no pending slot left for
    // gb, no in-process event would rebuild its rows — the rebuild itself
    // must re-enqueue it, and as ContentChanged: a reverted header reads
    // fresh by hash, which a deps-only slot would skip past.
    tmp.touch("gen.h",
              "#pragma once\n#ifdef MODE\nint gen_mode();\n#endif\n"
              "inline int gen_fn() { return 2; }\n");
    auto a2 = index_file(tmp, tmp.path("ga.cpp"));
    ASSERT_FALSE(a2.data.empty());
    merge(a2.data.data(), a2.data.size());
    auto header_id = workspace.path_pool.intern(tmp.path("gen.h"));
    ASSERT_EQ(workspace.shards[header_id].variants().size(), std::size_t(1));

    ASSERT_TRUE(pump.pending_reason(b_tu) == ReindexReason::ContentChanged);
    // ga's own fresh pin is stored: the rebuild must not re-enqueue it.
    ASSERT_FALSE(pump.pending_reason(workspace.path_pool.intern(a2.tu_path)).has_value());
}

TEST_CASE(RejectsCorruptSection) {
    TempDir tmp;
    tmp.touch("cor.h", "#pragma once\ninline int cor() { return 1; }\n");
    tmp.touch("cor_main.cpp", "#include \"cor.h\"\nint use_cor() { return cor(); }\n");

    auto indexed = index_file(tmp, tmp.path("cor_main.cpp"));
    ASSERT_FALSE(indexed.data.empty());

    // Corrupt the main file's blob bytes in place: the outer wire still
    // verifies (sections are opaque bytes to it), only the blob's byte
    // identity check against the recorded section hash fails.
    std::string corrupt = indexed.data;
    auto tampered = index::TUIndex::from_bytes(corrupt);
    ASSERT_TRUE(tampered.loaded());
    auto main_section = tampered.section_of(tampered.path_count() - 1);
    ASSERT_TRUE(main_section.has_value());
    auto blob = tampered.section_blob(*main_section);
    auto pos = llvm::StringRef(corrupt).find(blob);
    ASSERT_TRUE(pos != llvm::StringRef::npos);
    for(std::size_t i = 0; i < blob.size(); i += 1) {
        corrupt[pos + i] = 'X';
    }

    // The header section verifies fine and is staged before the main
    // section's identity check fails; the reject must discard the whole
    // result — a manifest whose recorded versions all match the disk would
    // otherwise be judged fresh forever with the main file's rows missing.
    merge(corrupt.data(), corrupt.size());
    auto tu_id = workspace.path_pool.intern(indexed.tu_path);
    auto header_id = workspace.path_pool.intern(tmp.path("cor.h"));
    ASSERT_FALSE(workspace.project_index.manifests.contains(tu_id));
    ASSERT_FALSE(workspace.shards.contains(header_id));
    // No global trace either: symbol identities from an untrusted result
    // would stay canonical for their hashes forever (later merges only
    // fill empty names), and stray FileVersions would persist with the
    // next save.
    ASSERT_TRUE(workspace.project_index.symbols.empty());
    ASSERT_TRUE(workspace.project_index.file_versions.empty());

    // The intact result still lands afterwards.
    merge(indexed.data.data(), indexed.data.size());
    ASSERT_TRUE(workspace.project_index.manifests.contains(tu_id));
    ASSERT_TRUE(workspace.shards.contains(header_id));
}

TEST_CASE(HashlessRemergeHits) {
    TempDir tmp;
    tmp.touch("pcm.cpp", "int hashless_fn() { return 7; }\n");
    auto src = tmp.path("pcm.cpp");
    auto indexed = index_file(tmp, src);
    ASSERT_FALSE(indexed.data.empty());

    // A file behind a PCM ships no consumed-content hash; the variant
    // identity is the blob's own byte hash, so membership needs no
    // content vouching at all.
    auto wire = strip_path_hashes(indexed.data);
    ASSERT_FALSE(wire.empty());

    merge(wire.data(), wire.size());
    auto path_id = workspace.path_pool.intern(src);
    ASSERT_EQ(workspace.shards[path_id].variants().size(), std::size_t(1));

    // Re-merging the same rows must register as a hit, not append the
    // stored variant to the blob a second time.
    merge(wire.data(), wire.size());
    ASSERT_EQ(workspace.shards[path_id].variants().size(), std::size_t(1));
}

TEST_CASE(FailedWriteNotCounted) {
    TempDir tmp;
    tmp.touch("main.cpp", "int uncommitted() { return 1; }\n");
    auto src = tmp.path("main.cpp");

    // A storage whose commits never land (disk full, permissions): the
    // gauge must report what was durably committed, not what the save
    // attempted.
    struct FailingStorage final : index::BlobDatabase {
        index::ReadBlob read(index::IndexBlobKind, llvm::StringRef) override {
            return {};
        }

        bool contains(index::IndexBlobKind, llvm::StringRef) override {
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

        void for_each_key(index::IndexBlobKind,
                          llvm::function_ref<void(llvm::StringRef)>) override {}

        std::expected<std::uint64_t, std::string> advance_read_snapshot() override {
            return 0;
        }

        void retire_old_snapshot() override {}

        std::expected<bool, std::string> grow() override {
            return false;
        }
    };

    workspace.index_db = std::make_unique<FailingStorage>();

    auto indexed = index_file(tmp, src);
    ASSERT_FALSE(indexed.data.empty());
    merge(indexed.data.data(), indexed.data.size());
    ASSERT_EQ(index_store.pending_shard_writes(), 1u);

    auto save = [&] {
        auto body = [&]() -> kota::task<> {
            co_await async_save();
        };
        auto task = body();
        loop.schedule(task);
        loop.run();
    };
    save();
    ASSERT_EQ(index_store.last_save_shards(), 0u);
    // The failed batch is re-dirtied rather than discarded, so a later
    // save has it to retry and the cache converges once the storage
    // recovers.
    ASSERT_EQ(index_store.pending_shard_writes(), 1u);

    open_store(tmp, workspace);
    save();
    ASSERT_EQ(index_store.last_save_shards(), 1u);
    ASSERT_EQ(index_store.pending_shard_writes(), 0u);
}

TEST_CASE(WriteCorruptionRebuildsDatabase) {
    TempDir tmp;
    tmp.touch("clean.cpp", "int clean_value() { return 1; }\n");
    tmp.touch("dirty.cpp", "int dirty_value() { return 2; }\n");
    open_store(tmp, workspace);

    // Corruption surfacing at write time (a damaged page only the write's
    // tree descent reaches): the save must condemn the environment and
    // continue on a fresh one instead of re-writing into it every save.
    // Batch shards own their bytes and stay to re-persist; the clean
    // resident view is shed and its owner re-enqueued.
    struct CorruptOnWrite final : index::BlobDatabase {
        bool* condemned;
        bool fail = false;
        bool poisoned = false;

        index::ReadBlob read(index::IndexBlobKind, llvm::StringRef) override {
            return {};
        }

        bool contains(index::IndexBlobKind, llvm::StringRef) override {
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

        void for_each_key(index::IndexBlobKind,
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
    workspace.index_db = std::move(spy);

    auto indexed_clean = index_file(tmp, tmp.path("clean.cpp"));
    auto indexed_dirty = index_file(tmp, tmp.path("dirty.cpp"));
    ASSERT_FALSE(indexed_clean.data.empty());
    ASSERT_FALSE(indexed_dirty.data.empty());

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

    ASSERT_TRUE(condemned);
    ASSERT_TRUE(workspace.index_db != nullptr);
    auto clean_id = workspace.path_pool.intern(indexed_clean.tu_path);
    auto dirty_id = workspace.path_pool.intern(indexed_dirty.tu_path);
    ASSERT_FALSE(workspace.shards.contains(clean_id));
    ASSERT_TRUE(workspace.shards.contains(dirty_id));
    ASSERT_TRUE(pump.pending_reason(clean_id) == ReindexReason::ContentChanged);
    ASSERT_EQ(index_store.last_save_shards(), 0u);

    // The next save re-persists everything servable into the fresh database.
    save();
    ASSERT_EQ(index_store.last_save_shards(), 1u);
    ASSERT_FALSE(index_store.has_unsaved_state());
}

TEST_CASE(MigrationCorruptionRebuildsDatabase) {
    TempDir tmp;
    tmp.touch("main.cpp", "int migrate_value() { return 1; }\n");
    open_store(tmp, workspace);

    // Corruption surfacing first at migration time (a damaged page only the
    // re-read from the advanced snapshot reaches, after the write-time
    // check passed): same recovery as write-time corruption — the resident
    // view is shed with its owner re-enqueued, the environment condemned
    // and replaced by a fresh one.
    struct CorruptOnRead final : index::BlobDatabase {
        bool* condemned;
        bool poisoned = false;

        index::ReadBlob read(index::IndexBlobKind, llvm::StringRef) override {
            poisoned = true;
            return {};
        }

        bool contains(index::IndexBlobKind, llvm::StringRef) override {
            return false;
        }

        llvm::SmallVector<std::size_t> write(llvm::ArrayRef<Blob>,
                                             llvm::ArrayRef<index::BlobKey>) override {
            return {};
        }

        void for_each_key(index::IndexBlobKind,
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
    workspace.index_db = std::move(spy);

    auto indexed = index_file(tmp, tmp.path("main.cpp"));
    ASSERT_FALSE(indexed.data.empty());
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

    auto path_id = workspace.path_pool.intern(indexed.tu_path);
    ASSERT_TRUE(condemned);
    ASSERT_TRUE(workspace.index_db != nullptr);
    ASSERT_FALSE(workspace.shards.contains(path_id));
    ASSERT_TRUE(pump.pending_reason(path_id) == ReindexReason::ContentChanged);
    ASSERT_EQ(index_store.last_save_shards(), 0u);

    // The re-dirtied manifests, global and CDB snapshot re-persist into
    // the fresh database.
    save();
    ASSERT_FALSE(index_store.has_unsaved_state());
}

TEST_CASE(WriteFailureStopsBatch) {
    TempDir tmp;
    open_store(tmp, workspace);
    auto& db = *workspace.index_db;

    // Wedge the manifest's destination with a non-empty directory so its
    // commit fails while the shard before it lands.
    tmp.touch("cache/cache/v1/index-manifest/k.idx/wedge");

    auto failures = db.write(
        {
            {index::IndexBlobKind::Shard,    "k",      "shard bytes"   },
            {index::IndexBlobKind::Manifest, "k",      "manifest bytes"},
            {index::IndexBlobKind::Global,   "global", "global bytes"  },
    },
        {});

    // The failure fails the rest of the batch: a global must never land
    // above a manifest that did not.
    ASSERT_EQ(failures.size(), 2u);
    ASSERT_EQ(failures[0], 1u);
    ASSERT_EQ(failures[1], 2u);
    ASSERT_TRUE(db.contains(index::IndexBlobKind::Shard, "k"));
    ASSERT_FALSE(db.contains(index::IndexBlobKind::Global, "global"));
}

};  // TEST_SUITE(IndexerMerge)

TEST_SUITE(IndexerStaleness) {

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
        auto indexed = index_file(tmp, src);
        if(indexed.data.empty()) {
            return false;
        }
        f.merge(indexed.data.data(), indexed.data.size());
        return true;
    }
};

TEST_CASE(TouchRepairsStamp) {
    Indexed x;
    ASSERT_TRUE(x.setup());
    ASSERT_FALSE(x.f.need_update(x.src));

    // Same bytes, new mtime: the stat fast path misses, the hash proves a
    // mere touch, and the stamp is repaired in place (dirtying the global
    // blob so the repair persists).
    ASSERT_TRUE(set_file_mtime(x.header, file_mtime_ns(x.header) + 5'000'000'000));
    x.f.reset_global_dirty();
    x.f.clear_verdicts();
    ASSERT_FALSE(x.f.need_update(x.src));
    ASSERT_TRUE(x.f.global_dirty());
}

TEST_CASE(PreservedMtimeEditStale) {
    Indexed x;
    ASSERT_TRUE(x.setup());
    auto recorded = file_mtime_ns(x.header);

    // Different content restored to the recorded mtime (rsync -t, git
    // restore-mtime): equality of the stat is not enough — the size moved,
    // and the hash check must catch the edit.
    x.tmp.touch("dep.h", "#pragma once\ninline int dep() { return 12345; }\n");
    ASSERT_TRUE(set_file_mtime(x.header, recorded));
    x.f.clear_verdicts();
    ASSERT_TRUE(x.f.need_update(x.src));
}

TEST_CASE(AllDepsChecked) {
    TempDir tmp;
    tmp.touch("first.h", "#pragma once\ninline int first() { return 1; }\n");
    tmp.touch("second.h", "#pragma once\ninline int second() { return 2; }\n");
    tmp.touch("main.cpp",
              "#include \"first.h\"\n#include \"second.h\"\n"
              "int use() { return first() + second(); }\n");
    IndexerFixture f;
    auto indexed = index_file(tmp, tmp.path("main.cpp"));
    ASSERT_FALSE(indexed.data.empty());
    f.merge(indexed.data.data(), indexed.data.size());
    ASSERT_FALSE(f.need_update(tmp.path("main.cpp")));

    // Only the second dependency changes; a partial iteration would call
    // the TU fresh.
    auto recorded = file_mtime_ns(tmp.path("second.h"));
    tmp.touch("second.h", "#pragma once\ninline int second() { return 22222; }\n");
    ASSERT_TRUE(set_file_mtime(tmp.path("second.h"), recorded));
    f.clear_verdicts();
    ASSERT_TRUE(f.need_update(tmp.path("main.cpp")));
}

};  // TEST_SUITE(IndexerStaleness)

TEST_SUITE(IndexerLoad) {

TEST_CASE(LoadRestoresIndex) {
    TempDir tmp;
    tmp.touch("dep.h", "#pragma once\ninline int dep() { return 1; }\n");
    tmp.touch("main.cpp", "#include \"dep.h\"\nint use() { return dep(); }\n");
    auto src = tmp.path("main.cpp");

    {
        IndexerFixture f;
        open_store(tmp, f.workspace);
        auto indexed = index_file(tmp, src);
        ASSERT_FALSE(indexed.data.empty());
        f.merge(indexed.data.data(), indexed.data.size());
        f.save();
    }

    IndexerFixture f;
    open_store(tmp, f.workspace);
    f.load();

    auto tu_id = f.workspace.path_pool.intern(src);
    auto header_id = f.workspace.path_pool.intern(tmp.path("dep.h"));
    ASSERT_TRUE(f.workspace.shards.contains(tu_id));
    ASSERT_TRUE(f.workspace.shards.contains(header_id));
    ASSERT_TRUE(f.workspace.project_index.contributions.lookup(header_id).contains(tu_id));
    // The persisted FileVersion stamps make the untouched TU judge fresh
    // without any reindex.
    ASSERT_FALSE(f.need_update(src));
}

TEST_CASE(LoadHealsBrokenShard) {
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
        open_store(tmp, f.workspace);
        auto indexed = index_file(tmp, src);
        ASSERT_FALSE(indexed.data.empty());
        f.merge(indexed.data.data(), indexed.data.size());
        f.save();
        header_key = blob_key(
            f.workspace.path_pool.resolve(f.workspace.path_pool.intern(tmp.path("dep.h"))));
    }

    // Corrupt the header's blob and plant an orphan nothing references
    // (the store's layout is {root}/cache/v{N}, under the "cache" root).
    tmp.touch("cache/cache/v1/index/" + header_key + ".idx", "corrupted beyond verification");
    tmp.touch("cache/cache/v1/index/deadbeefdeadbeef.idx", "orphan");

    IndexerFixture f;
    open_store(tmp, f.workspace);
    f.load();

    // The header's rows are unservable, so its contributing TU's manifest
    // is dropped and the TU re-enqueued — no CDB entry would ever re-index
    // a header otherwise. The orphan is swept.
    auto tu_id = f.workspace.path_pool.intern(src);
    ASSERT_TRUE(f.workspace.project_index.manifests.empty());
    ASSERT_TRUE(f.pump.pending_reason(tu_id).has_value());

    // The dropped manifest also retired the TU's contribution to the
    // OTHER header: its loaded shard's live mask must follow, or it keeps
    // serving a variant nothing contributes any more.
    auto extra_id = f.workspace.path_pool.intern(tmp.path("extra.h"));
    auto extra_it = f.workspace.shards.find(extra_id);
    ASSERT_TRUE(extra_it != f.workspace.shards.end());
    ASSERT_TRUE(extra_it->second.has_dead_variants());

    // Load defers blob cleanup into the first save (no synchronous
    // database commits on the startup event loop); the orphan dies there.
    auto save_body = [&]() -> kota::task<> {
        co_await f.async_save();
    };
    auto task = save_body();
    f.loop.schedule(task);
    f.loop.run();
    bool orphan_alive = false;
    f.workspace.index_db->for_each_key(index::IndexBlobKind::Shard, [&](llvm::StringRef key) {
        orphan_alive |= key == "deadbeefdeadbeef";
    });
    ASSERT_FALSE(orphan_alive);
}

TEST_CASE(ReadOnlyLoadKeepsDisk) {
    TempDir tmp;
    tmp.touch("dep.h", "#pragma once\ninline int dep() { return 1; }\n");
    tmp.touch("main.cpp", "#include \"dep.h\"\nint use() { return dep(); }\n");
    auto src = tmp.path("main.cpp");
    std::string header_key;
    std::string manifest_key;

    {
        IndexerFixture f;
        open_store(tmp, f.workspace);
        auto indexed = index_file(tmp, src);
        ASSERT_FALSE(indexed.data.empty());
        f.merge(indexed.data.data(), indexed.data.size());
        f.save();
        header_key = blob_key(
            f.workspace.path_pool.resolve(f.workspace.path_pool.intern(tmp.path("dep.h"))));
        manifest_key = blob_key(f.workspace.path_pool.resolve(f.workspace.path_pool.intern(src)));
    }

    tmp.touch("cache/cache/v1/index/" + header_key + ".idx", "corrupted beyond verification");
    tmp.touch("cache/cache/v1/index/deadbeefdeadbeef.idx", "orphan");

    IndexerFixture f;
    open_store(tmp, f.workspace);
    f.load(/*read_only=*/true);

    // The in-memory sweeps still run: the unservable header drops its
    // contributing TU's manifest and re-enqueues the TU.
    auto tu_id = f.workspace.path_pool.intern(src);
    ASSERT_TRUE(f.workspace.project_index.manifests.empty());
    ASSERT_TRUE(f.pump.pending_reason(tu_id).has_value());

    // But every blob survives on disk — a server running concurrently may
    // still reference what this reader judged stale.
    bool header_alive = false, orphan_alive = false, manifest_alive = false;
    f.workspace.index_db->for_each_key(index::IndexBlobKind::Shard, [&](llvm::StringRef key) {
        header_alive |= key == header_key;
        orphan_alive |= key == "deadbeefdeadbeef";
    });
    f.workspace.index_db->for_each_key(index::IndexBlobKind::Manifest, [&](llvm::StringRef key) {
        manifest_alive |= key == manifest_key;
    });
    ASSERT_TRUE(header_alive);
    ASSERT_TRUE(orphan_alive);
    ASSERT_TRUE(manifest_alive);
}

TEST_CASE(LoadHealsMissingVariant) {
    TempDir tmp;
    tmp.touch("dep.h", "#pragma once\ninline int dep() { return 1; }\n");
    tmp.touch("main.cpp", "#include \"dep.h\"\nint use() { return dep(); }\n");
    auto src = tmp.path("main.cpp");
    std::string header_key;

    {
        IndexerFixture f;
        open_store(tmp, f.workspace);
        auto indexed = index_file(tmp, src);
        ASSERT_FALSE(indexed.data.empty());
        f.merge(indexed.data.data(), indexed.data.size());
        f.save();
        header_key = blob_key(
            f.workspace.path_pool.resolve(f.workspace.path_pool.intern(tmp.path("dep.h"))));
    }

    // Replace the header's blob with one that verifies but stores a variant
    // no manifest contributed — the residue of a crash or failed write that
    // landed the manifest without its shard.
    tmp.touch("cache/cache/v1/index/" + header_key + ".idx",
              planted_blob("#pragma once\ninline int dep() { return 1; }\n", 0x1234));

    IndexerFixture f;
    open_store(tmp, f.workspace);
    f.load();

    // set_live would silently drop the missing rows, so the shard is as
    // unservable as an unreadable one: the TU's manifest goes and the TU
    // re-enqueues.
    auto tu_id = f.workspace.path_pool.intern(src);
    ASSERT_TRUE(f.workspace.project_index.manifests.empty());
    ASSERT_TRUE(f.pump.pending_reason(tu_id).has_value());
}

TEST_CASE(LoadHealsWrongGeneration) {
    TempDir tmp;
    tmp.touch("dep.h", "#pragma once\ninline int dep() { return 1; }\n");
    tmp.touch("main.cpp", "#include \"dep.h\"\nint use() { return dep(); }\n");
    auto src = tmp.path("main.cpp");
    std::string header_key;
    std::uint64_t rows_hash = 0;

    {
        IndexerFixture f;
        open_store(tmp, f.workspace);
        auto indexed = index_file(tmp, src);
        ASSERT_FALSE(indexed.data.empty());
        f.merge(indexed.data.data(), indexed.data.size());
        f.save();
        auto header_id = f.workspace.path_pool.intern(tmp.path("dep.h"));
        auto tu_id = f.workspace.path_pool.intern(src);
        rows_hash = f.workspace.project_index.contributions.lookup(header_id).lookup(tu_id);
        ASSERT_TRUE(rows_hash != 0);
        header_key = blob_key(f.workspace.path_pool.resolve(header_id));
    }

    // Replace the header's blob with one from ANOTHER content generation
    // whose explicit variant list still claims the contributed identity —
    // the residue of a crash between shard and manifest writes. Every
    // recorded FileVersion matches the disk, so only the generation pin
    // can tell that positions would map through stale text.
    tmp.touch("cache/cache/v1/index/" + header_key + ".idx", planted_blob("stale text", rows_hash));

    IndexerFixture f;
    open_store(tmp, f.workspace);
    f.load();

    auto tu_id = f.workspace.path_pool.intern(src);
    ASSERT_TRUE(f.workspace.project_index.manifests.empty());
    ASSERT_TRUE(f.pump.pending_reason(tu_id).has_value());
}

TEST_CASE(LoadDropsNewerManifest) {
    TempDir tmp;
    tmp.touch("main.cpp", "int lone() { return 1; }\n");
    auto src = tmp.path("main.cpp");

    {
        IndexerFixture f;
        open_store(tmp, f.workspace);
        auto indexed = index_file(tmp, src);
        ASSERT_FALSE(indexed.data.empty());
        f.merge(indexed.data.data(), indexed.data.size());
        f.save();

        // Plant what a lost global write leaves behind: a manifest stamped
        // with a generation the persisted global never reached. Every
        // FileVersion it references is known and its shard variant stored
        // (a rows-only reindex), so only the stamp can tell that the
        // global's symbols never landed.
        auto tu_id = f.workspace.path_pool.intern(src);
        auto raced = f.workspace.project_index.manifests.find(tu_id)->second;
        raced.global_gen = f.workspace.project_index.global_generation + 1;
        std::string bytes;
        llvm::raw_string_ostream os(bytes);
        index::serialize_manifest(raced, os);
        // Keyed by the interned (canonical) spelling, like save() itself:
        // on Windows the raw TempDir spelling hashes to a different key.
        f.workspace.index_db->write(
            {
                {index::IndexBlobKind::Manifest,
                 blob_key(f.workspace.path_pool.resolve(tu_id)),
                 std::move(bytes)}
        },
            {});
    }

    IndexerFixture f;
    open_store(tmp, f.workspace);
    f.load();

    // The raced manifest is dropped and its TU re-enqueued; the reindex
    // rewrites the manifest and the global together.
    auto tu_id = f.workspace.path_pool.intern(src);
    ASSERT_TRUE(f.workspace.project_index.manifests.empty());
    ASSERT_TRUE(f.pump.pending_reason(tu_id) == ReindexReason::ContentChanged);
}

TEST_CASE(LoadDropsLostManifest) {
    TempDir tmp;
    tmp.touch("main.cpp", "int lone() { return 1; }\n");
    auto src = tmp.path("main.cpp");

    {
        IndexerFixture f;
        open_store(tmp, f.workspace);
        auto indexed = index_file(tmp, src);
        ASSERT_FALSE(indexed.data.empty());
        f.merge(indexed.data.data(), indexed.data.size());
        f.save();

        // Plant what a failed manifest write under a landed global leaves
        // behind: the previous manifest, older-stamped, with every
        // FileVersion still resolvable and its shard variant stored (a
        // reindex that changed rows or the include tree only). Only the
        // global's pin can tell it is not the manifest the save meant.
        auto tu_id = f.workspace.path_pool.intern(src);
        auto lost = f.workspace.project_index.manifests.find(tu_id)->second;
        lost.global_gen = f.workspace.project_index.global_generation - 1;
        std::string bytes;
        llvm::raw_string_ostream os(bytes);
        index::serialize_manifest(lost, os);
        f.workspace.index_db->write(
            {
                {index::IndexBlobKind::Manifest,
                 blob_key(f.workspace.path_pool.resolve(tu_id)),
                 std::move(bytes)}
        },
            {});
    }

    IndexerFixture f;
    open_store(tmp, f.workspace);
    f.load();

    // The mistamped manifest is dropped and the TU re-enqueued instead of
    // the previous reindex's dependency set and rows serving as current.
    auto tu_id = f.workspace.path_pool.intern(src);
    ASSERT_TRUE(f.workspace.project_index.manifests.empty());
    ASSERT_TRUE(f.pump.pending_reason(tu_id) == ReindexReason::ContentChanged);
}

TEST_CASE(LoadRequeuesStaleManifest) {
    TempDir tmp;
    tmp.touch("dep.h", "#pragma once\ninline int dep() { return 1; }\n");
    tmp.touch("main.cpp", "#include \"dep.h\"\nint use() { return dep(); }\n");
    auto src = tmp.path("main.cpp");
    auto header = tmp.path("dep.h");

    {
        IndexerFixture f;
        open_store(tmp, f.workspace);
        auto indexed = index_file(tmp, src);
        ASSERT_FALSE(indexed.data.empty());
        f.merge(indexed.data.data(), indexed.data.size());
        f.save();

        // Plant what a crash between save phases leaves: a manifest whose
        // dependency FileVersion the persisted global table never learned,
        // while the TU's own version is known — here for the header, whose
        // standalone index no CDB sweep would ever rebuild.
        auto header_id = f.workspace.path_pool.intern(header);
        std::uint32_t header_fv = ~0u;
        for(auto& [fv, record]: f.workspace.project_index.file_versions) {
            if(record.path_id == header_id) {
                header_fv = fv;
            }
        }
        ASSERT_TRUE(header_fv != ~0u);
        index::TUManifest stale;
        stale.tu_fv = header_fv;
        stale.nodes.push_back({.fv = 9999});
        std::string bytes;
        llvm::raw_string_ostream os(bytes);
        index::serialize_manifest(stale, os);
        f.workspace.index_db->write(
            {
                {index::IndexBlobKind::Manifest, blob_key(header), std::move(bytes)}
        },
            {});
    }

    IndexerFixture f;
    open_store(tmp, f.workspace);
    f.load();

    // The unresolvable manifest is dropped and its TU re-enqueued instead
    // of losing its persisted index forever; the blob itself dies at the
    // first save (load defers cleanup off the startup event loop).
    auto header_id = f.workspace.path_pool.intern(header);
    ASSERT_TRUE(f.pump.pending_reason(header_id) == ReindexReason::ContentChanged);
    ASSERT_FALSE(f.workspace.project_index.manifests.contains(header_id));
    auto save_body = [&]() -> kota::task<> {
        co_await f.async_save();
    };
    auto task = save_body();
    f.loop.schedule(task);
    f.loop.run();
    bool stale_alive = false;
    f.workspace.index_db->for_each_key(index::IndexBlobKind::Manifest, [&](llvm::StringRef key) {
        stale_alive |= key == blob_key(header);
    });
    ASSERT_FALSE(stale_alive);

    // The TU whose manifest resolved is untouched.
    auto tu_id = f.workspace.path_pool.intern(src);
    ASSERT_TRUE(f.workspace.project_index.manifests.contains(tu_id));
    ASSERT_FALSE(f.pump.pending_reason(tu_id).has_value());
}

TEST_CASE(DeferredSweepYieldsToFreshWrite) {
    TempDir tmp;
    tmp.touch("main.cpp", "int sweep_target() { return 1; }\n");
    auto src = tmp.path("main.cpp");

    {
        IndexerFixture f;
        open_store(tmp, f.workspace);
        auto indexed = index_file(tmp, src);
        ASSERT_FALSE(indexed.data.empty());
        f.merge(indexed.data.data(), indexed.data.size());
        f.save();
        // The indexer keys blobs by the pool-canonical path, which need
        // not equal the raw temp path byte-for-byte (Windows 8.3 names).
        auto key =
            blob_key(f.workspace.path_pool.resolve(f.workspace.path_pool.intern(indexed.tu_path)));
        // Replace the persisted manifest with an unresolvable one: the
        // next load sweeps it — deferred into the first save — and the
        // TU's shard turns orphan, deferred too.
        index::TUManifest stale;
        stale.tu_fv = 999999;
        stale.nodes.push_back({.fv = 9999});
        std::string bytes;
        llvm::raw_string_ostream os(bytes);
        index::serialize_manifest(stale, os);
        f.workspace.index_db->write(
            {
                {index::IndexBlobKind::Manifest, key, std::move(bytes)}
        },
            {});
    }

    IndexerFixture f;
    open_store(tmp, f.workspace);
    ASSERT_TRUE(f.load());

    // The swept TU re-indexes before the first save, so that save both
    // re-writes and (deferred) removes the same keys — the fresh write
    // must win or the file never persists again.
    auto indexed = index_file(tmp, src);
    ASSERT_FALSE(indexed.data.empty());
    f.merge(indexed.data.data(), indexed.data.size());
    f.save();

    auto key =
        blob_key(f.workspace.path_pool.resolve(f.workspace.path_pool.intern(indexed.tu_path)));
    bool manifest_alive = false;
    f.workspace.index_db->for_each_key(index::IndexBlobKind::Manifest,
                                       [&](llvm::StringRef k) { manifest_alive |= k == key; });
    bool shard_alive = false;
    f.workspace.index_db->for_each_key(index::IndexBlobKind::Shard,
                                       [&](llvm::StringRef k) { shard_alive |= k == key; });
    ASSERT_TRUE(manifest_alive);
    ASSERT_TRUE(shard_alive);
}

TEST_CASE(LmdbLoadServesAcrossSaves) {
    TempDir tmp;
    tmp.touch("main.cpp", "int lmdb_value() { return 1; }\n");
    auto src = tmp.path("main.cpp");

    auto open_lmdb = [&](Workspace& workspace) {
        auto store = CacheStore::open(tmp.path("cache"), 1);
        ASSERT_TRUE(store.has_value());
        workspace.store.emplace(std::move(*store));
        workspace.index_db = index::open_lmdb_database(*workspace.store);
        ASSERT_TRUE(workspace.index_db != nullptr);
    };

    {
        IndexerFixture f;
        open_lmdb(f.workspace);
        auto indexed = index_file(tmp, src);
        ASSERT_FALSE(indexed.data.empty());
        f.merge(indexed.data.data(), indexed.data.size());
        f.save();
    }

    IndexerFixture f;
    open_lmdb(f.workspace);
    ASSERT_TRUE(f.load());
    auto path_id = f.workspace.path_pool.intern(src);
    ASSERT_TRUE(f.workspace.shards.contains(path_id));

    // The loaded shard borrows the open-time snapshot. A save that commits
    // anything advances and retires it — the shard must come out rebound
    // onto the fresh snapshot, still serving.
    tmp.touch("other.cpp", "int other_value() { return 2; }\n");
    auto other = index_file(tmp, tmp.path("other.cpp"));
    ASSERT_FALSE(other.data.empty());
    f.merge(other.data.data(), other.data.size());
    f.save();

    auto it = f.workspace.shards.find(path_id);
    ASSERT_TRUE(it != f.workspace.shards.end());
    ASSERT_TRUE(it->second.loaded());
    ASSERT_EQ(it->second.content_hash(), llvm::xxh3_64bits("int lmdb_value() { return 1; }\n"));
    ASSERT_FALSE(it->second.bytes().empty());
}

TEST_CASE(CorruptGlobalCondemnsDatabase) {
    // Page corruption under the global blob: read fails, contains() still
    // says present, corrupted() confirms. load must condemn the database
    // (deleted on close) and continue on a fresh empty one instead of
    // parking in the disabled-persistence limbo forever.
    struct CorruptGlobal final : index::BlobDatabase {
        bool* condemned;

        index::ReadBlob read(index::IndexBlobKind, llvm::StringRef) override {
            return {};
        }

        bool contains(index::IndexBlobKind, llvm::StringRef) override {
            return true;
        }

        llvm::SmallVector<std::size_t> write(llvm::ArrayRef<Blob>,
                                             llvm::ArrayRef<index::BlobKey>) override {
            return {};
        }

        void for_each_key(index::IndexBlobKind,
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
    open_store(tmp, f.workspace);
    bool condemned = false;
    auto spy = std::make_unique<CorruptGlobal>();
    spy->condemned = &condemned;
    f.workspace.index_db = std::move(spy);

    ASSERT_TRUE(f.load());
    ASSERT_TRUE(condemned);
    ASSERT_TRUE(f.workspace.index_db != nullptr);
}

TEST_CASE(CorruptShardCondemnsDatabase) {
    TempDir tmp;
    tmp.touch("main.cpp", "int gone() { return 1; }\n");
    auto src = tmp.path("main.cpp");

    std::string tu_path;
    {
        IndexerFixture f;
        open_store(tmp, f.workspace);
        auto indexed = index_file(tmp, src);
        ASSERT_FALSE(indexed.data.empty());
        f.merge(indexed.data.data(), indexed.data.size());
        tu_path = indexed.tu_path;
        f.save();
    }

    // Corruption latched past the global anchor: the shard read poisons
    // the latch while the global stays readable. load must condemn here
    // too, and unwind everything it adopted — the views would otherwise
    // borrow from the condemned environment.
    struct CorruptShard final : index::BlobDatabase {
        std::unique_ptr<index::BlobDatabase> real;
        bool* condemned;
        bool poisoned = false;

        index::ReadBlob read(index::IndexBlobKind kind, llvm::StringRef key) override {
            if(kind == index::IndexBlobKind::Shard) {
                poisoned = true;
                return {};
            }
            return real->read(kind, key);
        }

        bool contains(index::IndexBlobKind kind, llvm::StringRef key) override {
            return real->contains(kind, key);
        }

        llvm::SmallVector<std::size_t> write(llvm::ArrayRef<Blob> puts,
                                             llvm::ArrayRef<index::BlobKey> removes) override {
            return real->write(puts, removes);
        }

        void for_each_key(index::IndexBlobKind kind,
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
    open_store(tmp, f.workspace);
    bool condemned = false;
    auto wrapper = std::make_unique<CorruptShard>();
    wrapper->real = std::move(f.workspace.index_db);
    wrapper->condemned = &condemned;
    f.workspace.index_db = std::move(wrapper);

    ASSERT_TRUE(f.load());
    ASSERT_TRUE(condemned);
    ASSERT_TRUE(f.workspace.shards.empty());
    ASSERT_TRUE(f.workspace.project_index.symbols.empty());

    // The TU has no CDB entry, so nothing else records the debt: it is
    // re-enqueued before the adopted state unwinds, and the fresh
    // database's first save persists it as standalone debt.
    ASSERT_TRUE(f.pump.pending_reason(f.workspace.path_pool.intern(tu_path)) ==
                ReindexReason::ContentChanged);
    ASSERT_TRUE(f.workspace.index_db != nullptr);
    f.save();
    auto snapshot = f.workspace.index_db->read(index::IndexBlobKind::CDB, "cdb");
    ASSERT_TRUE(snapshot);
    ASSERT_TRUE(snapshot.buffer->getBuffer().contains("main.cpp"));
}

TEST_CASE(UnreadableGlobalPreserved) {
    TempDir tmp;
    tmp.touch("main.cpp", "int keep() { return 1; }\n");
    auto src = tmp.path("main.cpp");

    {
        IndexerFixture f;
        open_store(tmp, f.workspace);
        auto indexed = index_file(tmp, src);
        ASSERT_FALSE(indexed.data.empty());
        f.merge(indexed.data.data(), indexed.data.size());
        f.save();
    }

    // The global blob exists but fails to open — a transient IO error at
    // startup, not absence. Sweeping would destroy the intact index; a
    // fresh lineage saved over the unread one could alias its fv ids and
    // generation stamps. The session must run memory-only and leave every
    // blob for the next start.
    struct UnreadableGlobal final : index::BlobDatabase {
        std::unique_ptr<index::BlobDatabase> real;

        index::ReadBlob read(index::IndexBlobKind kind, llvm::StringRef key) override {
            return kind == index::IndexBlobKind::Global ? index::ReadBlob{} : real->read(kind, key);
        }

        bool contains(index::IndexBlobKind kind, llvm::StringRef key) override {
            return real->contains(kind, key);
        }

        llvm::SmallVector<std::size_t> write(llvm::ArrayRef<Blob> puts,
                                             llvm::ArrayRef<index::BlobKey> removes) override {
            return real->write(puts, removes);
        }

        void for_each_key(index::IndexBlobKind kind,
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
        open_store(tmp, f.workspace);
        auto wrapper = std::make_unique<UnreadableGlobal>();
        wrapper->real = std::move(f.workspace.index_db);
        f.workspace.index_db = std::move(wrapper);
        f.load();
        ASSERT_TRUE(f.workspace.project_index.manifests.empty());
        ASSERT_TRUE(f.workspace.index_db == nullptr);
    }

    IndexerFixture f;
    open_store(tmp, f.workspace);
    f.load();
    ASSERT_FALSE(f.workspace.project_index.manifests.empty());
    ASSERT_FALSE(f.workspace.shards.empty());
}

TEST_CASE(DropIndexEvictsPersisted) {
    TempDir tmp;
    tmp.touch("dep.h", "#pragma once\ninline int dep() { return 1; }\n");
    tmp.touch("main.cpp", "#include \"dep.h\"\nint use() { return dep(); }\n");
    auto src = tmp.path("main.cpp");

    {
        IndexerFixture f;
        open_store(tmp, f.workspace);
        auto indexed = index_file(tmp, src);
        ASSERT_FALSE(indexed.data.empty());
        f.merge(indexed.data.data(), indexed.data.size());
        f.save();

        // The compile command changed: content freshness cannot see it, so
        // the TU's index is dropped wholesale and staleness flips at once.
        f.drop_index(f.workspace.path_pool.intern(src));
        ASSERT_TRUE(f.workspace.project_index.manifests.empty());
        ASSERT_TRUE(f.need_update(src));
        f.save();
    }

    // The drop survives a restart: nothing on disk resurrects the
    // old-command rows as fresh.
    IndexerFixture f;
    open_store(tmp, f.workspace);
    f.load();
    ASSERT_TRUE(f.workspace.project_index.manifests.empty());
    ASSERT_TRUE(f.workspace.shards.empty());
    ASSERT_TRUE(f.need_update(src));
}

TEST_CASE(OfflineCommandChangeReindexed) {
    TempDir tmp;
    tmp.touch("main.cpp", "int value() { return 1; }\n");
    auto src = tmp.path("main.cpp");

    {
        IndexerFixture f;
        open_store(tmp, f.workspace);
        f.workspace.cdb.add_command(tmp.root, src, llvm::StringRef("clang++ -DFOO=1 -c main.cpp"));
        auto indexed = index_file(tmp, src);
        ASSERT_FALSE(indexed.data.empty());
        f.merge(indexed.data.data(), indexed.data.size());
        f.save();
    }

    // The command changed while no server ran: content freshness cannot
    // see it, so the persisted CDB snapshot must catch it at load.
    IndexerFixture f;
    open_store(tmp, f.workspace);
    f.workspace.cdb.add_command(tmp.root, src, llvm::StringRef("clang++ -DFOO=2 -c main.cpp"));
    f.load();

    auto tu_id = f.workspace.path_pool.intern(src);
    ASSERT_FALSE(f.workspace.project_index.manifests.contains(tu_id));
    ASSERT_TRUE(f.pump.pending_reason(tu_id) == ReindexReason::ContentChanged);
}

TEST_CASE(UnchangedCommandKept) {
    TempDir tmp;
    tmp.touch("main.cpp", "int value() { return 1; }\n");
    auto src = tmp.path("main.cpp");

    {
        IndexerFixture f;
        open_store(tmp, f.workspace);
        f.workspace.cdb.add_command(tmp.root, src, llvm::StringRef("clang++ -DFOO=1 -c main.cpp"));
        auto indexed = index_file(tmp, src);
        ASSERT_FALSE(indexed.data.empty());
        f.merge(indexed.data.data(), indexed.data.size());
        f.save();
    }

    IndexerFixture f;
    open_store(tmp, f.workspace);
    f.workspace.cdb.add_command(tmp.root, src, llvm::StringRef("clang++ -DFOO=1 -c main.cpp"));
    f.load();

    auto tu_id = f.workspace.path_pool.intern(src);
    ASSERT_TRUE(f.workspace.project_index.manifests.contains(tu_id));
    ASSERT_FALSE(f.pump.pending_reason(tu_id).has_value());
}

TEST_CASE(RemovedEntryKeepsIndex) {
    TempDir tmp;
    tmp.touch("main.cpp", "int value() { return 1; }\n");
    auto src = tmp.path("main.cpp");

    {
        IndexerFixture f;
        open_store(tmp, f.workspace);
        f.workspace.cdb.add_command(tmp.root, src, llvm::StringRef("clang++ -DFOO=1 -c main.cpp"));
        auto indexed = index_file(tmp, src);
        ASSERT_FALSE(indexed.data.empty());
        f.merge(indexed.data.data(), indexed.data.size());
        f.save();
    }

    // The entry vanished from the CDB: the last-known rows still serve
    // navigation, same conservative semantics as the live reload path.
    IndexerFixture f;
    open_store(tmp, f.workspace);
    f.load();

    auto tu_id = f.workspace.path_pool.intern(src);
    ASSERT_TRUE(f.workspace.project_index.manifests.contains(tu_id));
    ASSERT_FALSE(f.pump.pending_reason(tu_id).has_value());
}

TEST_CASE(RuleChangeReindexed) {
    TempDir tmp;
    tmp.touch("main.cpp", "int value() { return 1; }\n");
    auto src = tmp.path("main.cpp");

    {
        IndexerFixture f;
        open_store(tmp, f.workspace);
        f.workspace.cdb.add_command(tmp.root, src, llvm::StringRef("clang++ -c main.cpp"));
        auto indexed = index_file(tmp, src);
        ASSERT_FALSE(indexed.data.empty());
        ASSERT_TRUE(f.merge(indexed.data.data(), indexed.data.size()));
        f.save();
    }

    // The CDB entry is unchanged, but a clice.toml rule now adjusts the
    // effective command — as invisible to content freshness as a command
    // edit, so the snapshot must cover it too.
    IndexerFixture f;
    open_store(tmp, f.workspace);
    f.workspace.cdb.add_command(tmp.root, src, llvm::StringRef("clang++ -c main.cpp"));
    f.workspace.config.rules.push_back(ConfigRule{
        .patterns = {"**/*.cpp"},
        .append = {"-DFOO=1"},
    });
    f.workspace.config.finalize(tmp.root);
    f.load();

    auto tu_id = f.workspace.path_pool.intern(src);
    ASSERT_FALSE(f.workspace.project_index.manifests.contains(tu_id));
    ASSERT_TRUE(f.pump.pending_reason(tu_id) == ReindexReason::ContentChanged);
}

TEST_CASE(HostChangeDropsHeader) {
    TempDir tmp;
    tmp.touch("dep.h", "#pragma once\ninline int dep() { return 1; }\n");
    tmp.touch("main.cpp", "#include \"dep.h\"\nint use() { return dep(); }\n");
    auto src = tmp.path("main.cpp");
    auto header = tmp.path("dep.h");

    {
        IndexerFixture f;
        open_store(tmp, f.workspace);
        f.workspace.cdb.add_command(tmp.root, src, llvm::StringRef("clang++ -DFOO=1 -c main.cpp"));
        // A standalone pass over the header, as a borrowed-context index
        // produces it; the host source itself was never indexed.
        auto indexed = index_file(tmp, header);
        ASSERT_FALSE(indexed.data.empty());
        ASSERT_TRUE(f.merge(indexed.data.data(), indexed.data.size()));
        f.save();
    }

    // The host's command changed while no server ran: the header has no
    // CDB entry of its own, so only include reachability from the changed
    // source can catch its borrowed-command manifest.
    IndexerFixture f;
    open_store(tmp, f.workspace);
    f.workspace.cdb.add_command(tmp.root, src, llvm::StringRef("clang++ -DFOO=2 -c main.cpp"));
    auto src_id = f.workspace.path_pool.intern(src);
    auto header_id = f.workspace.path_pool.intern(header);
    f.workspace.dep_graph.set_includes(src_id, 0, {header_id});
    f.workspace.dep_graph.build_reverse_map();
    f.load();

    ASSERT_FALSE(f.workspace.project_index.manifests.contains(header_id));
    ASSERT_TRUE(f.pump.pending_reason(header_id) == ReindexReason::ContentChanged);
}

TEST_CASE(HeaderRuleChangeReindexed) {
    TempDir tmp;
    tmp.touch("dep.h", "#pragma once\ninline int dep() { return 1; }\n");
    auto header = tmp.path("dep.h");

    {
        IndexerFixture f;
        open_store(tmp, f.workspace);
        auto indexed = index_file(tmp, header);
        ASSERT_FALSE(indexed.data.empty());
        ASSERT_TRUE(f.merge(indexed.data.data(), indexed.data.size()));
        f.save();
    }

    // A clice.toml rule matching the header itself changed offline: the
    // header has no CDB entry, so only its own snapshot entry can see it.
    IndexerFixture f;
    open_store(tmp, f.workspace);
    f.workspace.config.rules.push_back(ConfigRule{
        .patterns = {"**/*.h"},
        .append = {"-DFOO=1"},
    });
    f.workspace.config.finalize(tmp.root);
    f.load();

    auto header_id = f.workspace.path_pool.intern(header);
    ASSERT_FALSE(f.workspace.project_index.manifests.contains(header_id));
    ASSERT_TRUE(f.pump.pending_reason(header_id) == ReindexReason::ContentChanged);
}

TEST_CASE(RecordedHostChangeDrops) {
    TempDir tmp;
    tmp.touch("dep.h", "#pragma once\ninline int dep() { return 1; }\n");
    tmp.touch("main.cpp", "int use() { return 0; }\n");
    auto src = tmp.path("main.cpp");
    auto header = tmp.path("dep.h");

    {
        IndexerFixture f;
        open_store(tmp, f.workspace);
        f.workspace.cdb.add_command(tmp.root, src, llvm::StringRef("clang++ -DFOO=1 -c main.cpp"));
        auto indexed = index_file(tmp, header);
        ASSERT_FALSE(indexed.data.empty());
        ASSERT_TRUE(f.merge(indexed.data.data(), indexed.data.size()));
        f.set_header_host(f.workspace.path_pool.intern(header), f.workspace.path_pool.intern(src));
        f.save();
    }

    // The host's command changed offline AND the new command no longer
    // includes the header, so the rebuilt include graph cannot reach it —
    // only the recorded host association can catch the stale borrow.
    IndexerFixture f;
    open_store(tmp, f.workspace);
    f.workspace.cdb.add_command(tmp.root, src, llvm::StringRef("clang++ -DFOO=2 -c main.cpp"));
    f.load();

    auto header_id = f.workspace.path_pool.intern(header);
    ASSERT_FALSE(f.workspace.project_index.manifests.contains(header_id));
    ASSERT_TRUE(f.pump.pending_reason(header_id) == ReindexReason::ContentChanged);
}

TEST_CASE(PinnedHostKeepsHeader) {
    TempDir tmp;
    tmp.touch("dep.h", "#pragma once\ninline int dep() { return 1; }\n");
    tmp.touch("main.cpp", "#include \"dep.h\"\nint use() { return dep(); }\n");
    tmp.touch("other.cpp", "#include \"dep.h\"\nint more() { return dep(); }\n");
    auto src = tmp.path("main.cpp");
    auto other = tmp.path("other.cpp");
    auto header = tmp.path("dep.h");

    {
        IndexerFixture f;
        open_store(tmp, f.workspace);
        f.workspace.cdb.add_command(tmp.root, src, llvm::StringRef("clang++ -c main.cpp"));
        f.workspace.cdb.add_command(tmp.root, other, llvm::StringRef("clang++ -c other.cpp"));
        auto indexed = index_file(tmp, header);
        ASSERT_FALSE(indexed.data.empty());
        ASSERT_TRUE(f.merge(indexed.data.data(), indexed.data.size()));
        f.set_header_host(f.workspace.path_pool.intern(header), f.workspace.path_pool.intern(src));
        f.save();
    }

    // Another includer's command changed, but the header borrowed the
    // unchanged host's — the recorded association beats the reachability
    // approximation's over-drop.
    IndexerFixture f;
    open_store(tmp, f.workspace);
    f.workspace.cdb.add_command(tmp.root, src, llvm::StringRef("clang++ -c main.cpp"));
    f.workspace.cdb.add_command(tmp.root, other, llvm::StringRef("clang++ -DBAR=1 -c other.cpp"));
    auto src_id = f.workspace.path_pool.intern(src);
    auto other_id = f.workspace.path_pool.intern(other);
    auto header_id = f.workspace.path_pool.intern(header);
    f.workspace.dep_graph.set_includes(src_id, 0, {header_id});
    f.workspace.dep_graph.set_includes(other_id, 0, {header_id});
    f.workspace.dep_graph.build_reverse_map();
    f.load();

    ASSERT_TRUE(f.workspace.project_index.manifests.contains(header_id));
    ASSERT_FALSE(f.pump.pending_reason(header_id).has_value());
}

TEST_CASE(UnreachableHostRebuilds) {
    TempDir tmp;
    tmp.touch("dep.h", "#pragma once\ninline int dep() { return 1; }\n");
    tmp.touch("main.cpp", "int use() { return 0; }\n");
    auto src = tmp.path("main.cpp");
    auto header = tmp.path("dep.h");

    {
        IndexerFixture f;
        open_store(tmp, f.workspace);
        f.workspace.cdb.add_command(tmp.root, src, llvm::StringRef("clang++ -c main.cpp"));
        auto indexed = index_file(tmp, header);
        ASSERT_FALSE(indexed.data.empty());
        ASSERT_TRUE(f.merge(indexed.data.data(), indexed.data.size()));
        f.set_header_host(f.workspace.path_pool.intern(header), f.workspace.path_pool.intern(src));
        f.save();
    }

    // The host's command is untouched but an offline edit removed its
    // include of the header: the rows keep serving while a queued rebuild
    // re-selects a host.
    IndexerFixture f;
    open_store(tmp, f.workspace);
    f.workspace.cdb.add_command(tmp.root, src, llvm::StringRef("clang++ -c main.cpp"));
    f.load();

    auto header_id = f.workspace.path_pool.intern(header);
    ASSERT_TRUE(f.workspace.project_index.manifests.contains(header_id));
    ASSERT_TRUE(f.pump.pending_reason(header_id) == ReindexReason::ContentChanged);
}

TEST_CASE(CDBWriteFailureRetried) {
    TempDir tmp;
    tmp.touch("main.cpp", "int value() { return 1; }\n");
    auto src = tmp.path("main.cpp");

    // A storage that fails only the CDB snapshot blob, with everything
    // else landing normally.
    struct CDBFailingStorage final : index::BlobDatabase {
        std::unique_ptr<index::BlobDatabase> real;
        bool fail_cdb = true;
        llvm::SmallVector<index::IndexBlobKind> written;

        index::ReadBlob read(index::IndexBlobKind kind, llvm::StringRef key) override {
            return real->read(kind, key);
        }

        bool contains(index::IndexBlobKind kind, llvm::StringRef key) override {
            return real->contains(kind, key);
        }

        llvm::SmallVector<std::size_t> write(llvm::ArrayRef<Blob> puts,
                                             llvm::ArrayRef<index::BlobKey> removes) override {
            llvm::SmallVector<std::size_t> failed;
            for(std::size_t i = 0; i < puts.size(); i += 1) {
                written.push_back(puts[i].kind);
                if(fail_cdb && puts[i].kind == index::IndexBlobKind::CDB) {
                    failed.push_back(i);
                } else if(!real->write(llvm::ArrayRef(puts[i]), {}).empty()) {
                    failed.push_back(i);
                }
            }
            real->write({}, removes);
            return failed;
        }

        void for_each_key(index::IndexBlobKind kind,
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

    IndexerFixture f;
    open_store(tmp, f.workspace);
    f.workspace.cdb.add_command(tmp.root, src, llvm::StringRef("clang++ -c main.cpp"));
    auto failing = std::make_unique<CDBFailingStorage>();
    failing->real = std::move(f.workspace.index_db);
    auto* storage = failing.get();
    f.workspace.index_db = std::move(failing);

    auto indexed = index_file(tmp, src);
    ASSERT_FALSE(indexed.data.empty());
    ASSERT_TRUE(f.merge(indexed.data.data(), indexed.data.size()));
    f.save();

    // The snapshot rides the batch behind the index state it describes.
    ASSERT_EQ(int(storage->written.back()), int(index::IndexBlobKind::CDB));
    ASSERT_FALSE(storage->real->contains(index::IndexBlobKind::CDB, "cdb"));

    // Nothing else is dirty any more, yet the failed snapshot alone must
    // drive the next save until it lands — and until it does, the state
    // counts as unsaved (`clice index` fails on it after the final save).
    ASSERT_TRUE(f.index_store.has_unsaved_state());
    storage->fail_cdb = false;
    f.save();
    ASSERT_TRUE(storage->real->contains(index::IndexBlobKind::CDB, "cdb"));
    ASSERT_FALSE(f.index_store.has_unsaved_state());
}

TEST_CASE(MissingSnapshotRewritten) {
    TempDir tmp;
    tmp.touch("main.cpp", "int value() { return 1; }\n");
    auto src = tmp.path("main.cpp");

    {
        IndexerFixture f;
        open_store(tmp, f.workspace);
        f.workspace.cdb.add_command(tmp.root, src, llvm::StringRef("clang++ -c main.cpp"));
        auto indexed = index_file(tmp, src);
        ASSERT_FALSE(indexed.data.empty());
        ASSERT_TRUE(f.merge(indexed.data.data(), indexed.data.size()));
        f.save();
        // The global landed but the final CDB write never did: the rest of
        // the index is intact.
        f.workspace.index_db->write(
            {
        },
            {{index::IndexBlobKind::CDB, "cdb"}});
    }

    // A rerun that dirties nothing must still recreate the baseline —
    // without it, every later offline command edit would go undetected.
    IndexerFixture f;
    open_store(tmp, f.workspace);
    f.workspace.cdb.add_command(tmp.root, src, llvm::StringRef("clang++ -c main.cpp"));
    f.load();
    ASSERT_TRUE(f.index_store.has_unsaved_state());
    f.save();
    ASSERT_TRUE(f.workspace.index_db->contains(index::IndexBlobKind::CDB, "cdb"));
    ASSERT_FALSE(f.index_store.has_unsaved_state());
}

TEST_CASE(DroppedHeaderDebtRetried) {
    TempDir tmp;
    tmp.touch("dep.h", "#pragma once\ninline int dep() { return 1; }\n");
    tmp.touch("main.cpp", "int use() { return 0; }\n");
    auto src = tmp.path("main.cpp");
    auto header = tmp.path("dep.h");

    {
        IndexerFixture f;
        open_store(tmp, f.workspace);
        f.workspace.cdb.add_command(tmp.root, src, llvm::StringRef("clang++ -DFOO=1 -c main.cpp"));
        auto indexed = index_file(tmp, header);
        ASSERT_FALSE(indexed.data.empty());
        ASSERT_TRUE(f.merge(indexed.data.data(), indexed.data.size()));
        f.set_header_host(f.workspace.path_pool.intern(header), f.workspace.path_pool.intern(src));
        f.save();
    }

    {
        // The host's command changed offline: the header's index is dropped
        // and queued — but the rebuild never lands this session. The saved
        // snapshot must keep recording the header, or nothing would ever
        // retry it.
        IndexerFixture f;
        open_store(tmp, f.workspace);
        f.workspace.cdb.add_command(tmp.root, src, llvm::StringRef("clang++ -DFOO=2 -c main.cpp"));
        f.load();
        auto header_id = f.workspace.path_pool.intern(header);
        ASSERT_FALSE(f.workspace.project_index.manifests.contains(header_id));
        ASSERT_TRUE(f.pump.pending_reason(header_id) == ReindexReason::ContentChanged);
        f.save();
    }

    IndexerFixture f;
    open_store(tmp, f.workspace);
    f.workspace.cdb.add_command(tmp.root, src, llvm::StringRef("clang++ -DFOO=2 -c main.cpp"));
    f.load();

    auto header_id = f.workspace.path_pool.intern(header);
    ASSERT_FALSE(f.workspace.project_index.manifests.contains(header_id));
    ASSERT_TRUE(f.pump.pending_reason(header_id) == ReindexReason::ContentChanged);
}

TEST_CASE(VanishedHeaderDebtDies) {
    TempDir tmp;
    tmp.touch("dep.h", "#pragma once\ninline int dep() { return 1; }\n");
    tmp.touch("main.cpp", "int use() { return 0; }\n");
    auto src = tmp.path("main.cpp");
    auto header = tmp.path("dep.h");

    {
        IndexerFixture f;
        open_store(tmp, f.workspace);
        f.workspace.cdb.add_command(tmp.root, src, llvm::StringRef("clang++ -DFOO=1 -c main.cpp"));
        auto indexed = index_file(tmp, header);
        ASSERT_FALSE(indexed.data.empty());
        ASSERT_TRUE(f.merge(indexed.data.data(), indexed.data.size()));
        f.set_header_host(f.workspace.path_pool.intern(header), f.workspace.path_pool.intern(src));
        f.save();
    }

    {
        IndexerFixture f;
        open_store(tmp, f.workspace);
        f.workspace.cdb.add_command(tmp.root, src, llvm::StringRef("clang++ -DFOO=2 -c main.cpp"));
        f.load();
        f.save();
    }

    // The header was deleted while its debt entry sat in the snapshot: a
    // retry could never succeed, so the debt dies instead of keeping every
    // later run partial forever.
    ASSERT_TRUE(!llvm::sys::fs::remove(header));
    IndexerFixture f;
    open_store(tmp, f.workspace);
    f.workspace.cdb.add_command(tmp.root, src, llvm::StringRef("clang++ -DFOO=2 -c main.cpp"));
    f.load();
    ASSERT_FALSE(f.pump.pending_reason(f.workspace.path_pool.intern(header)).has_value());
}

};  // TEST_SUITE(IndexerLoad)

TEST_SUITE(IndexerRequeue) {

TEST_CASE(PreemptionKeepsBudget) {
    IndexerFixture f;
    auto id = f.workspace.path_pool.intern("/proj/a.cpp");
    f.pump.enqueue(id, ReindexReason::ContentChanged);

    // A preemption under memory pressure requeues without spending the
    // crash budget, no matter how often it repeats.
    for(unsigned i = 0; i < 2 * IndexerFixture::budget; ++i) {
        ASSERT_EQ(int(f.fail(id, /*crashed=*/false)), int(IndexerFixture::Verdict::Requeued));
    }
    ASSERT_EQ(f.attempts(id), 0u);
    ASSERT_TRUE(f.pump.pending_reason(id).has_value());
}

TEST_CASE(CrashSpendsBudget) {
    IndexerFixture f;
    auto id = f.workspace.path_pool.intern("/proj/poison.cpp");
    f.pump.enqueue(id, ReindexReason::ContentChanged);

    for(unsigned i = 0; i < IndexerFixture::budget; ++i) {
        ASSERT_EQ(int(f.fail(id, /*crashed=*/true)), int(IndexerFixture::Verdict::Requeued));
    }
    ASSERT_EQ(f.attempts(id), IndexerFixture::budget);

    // A preemption still requeues a file whose crash budget is spent:
    // dropping it would erase the pending state and serve the stale
    // shard as fresh. Only the next crash gives up.
    ASSERT_EQ(int(f.fail(id, /*crashed=*/false)), int(IndexerFixture::Verdict::Requeued));
    ASSERT_EQ(f.attempts(id), IndexerFixture::budget);

    // Giving up clears the pending slot: nothing is left to requeue, and
    // the stale shard serves as fresh — the accepted cost of abandoning.
    ASSERT_EQ(int(f.fail(id, /*crashed=*/true)), int(IndexerFixture::Verdict::GaveUp));
    ASSERT_FALSE(f.pump.pending_reason(id).has_value());
    ASSERT_EQ(int(f.fail(id, /*crashed=*/true)), int(IndexerFixture::Verdict::Dropped));
}

TEST_CASE(StaleCrashKeepsBudget) {
    IndexerFixture f;
    auto id = f.workspace.path_pool.intern("/proj/edited.cpp");
    f.pump.enqueue(id, ReindexReason::ContentChanged);
    auto stale = f.ticket(id);

    // The user fixes the file while the old bytes' dispatch is in flight:
    // the stale crash must not spend the fixed content's budget or touch
    // its pending slot.
    f.pump.enqueue(id, ReindexReason::ContentChanged);
    ASSERT_EQ(int(f.fail_at(id, stale, /*crashed=*/true)),
              int(IndexerFixture::Verdict::Superseded));
    ASSERT_EQ(f.attempts(id), 0u);
    ASSERT_TRUE(f.pump.pending_reason(id).has_value());
}

TEST_CASE(DepsDowngradeKeepsDebt) {
    IndexerFixture f;
    auto id = f.workspace.path_pool.intern("/proj/c.cpp");
    f.pump.enqueue(id, ReindexReason::ContentChanged);
    auto launch = f.ticket(id);

    // The content pass is dispatched; a deps-only cascade lands mid-flight
    // and downgrades the pending reason, betting on that pass to cover the
    // edit. The pass fails — the requeue must restore the ContentChanged
    // debt or the stale shard stops being suppressed.
    f.consume(id);
    f.pump.enqueue(id, ReindexReason::DepsOnly);
    ASSERT_EQ(int(*f.pump.pending_reason(id)), int(ReindexReason::DepsOnly));

    ASSERT_EQ(int(f.fail_at(id, launch, /*crashed=*/true)), int(IndexerFixture::Verdict::Requeued));
    ASSERT_EQ(int(*f.pump.pending_reason(id)), int(ReindexReason::ContentChanged));
    ASSERT_EQ(f.attempts(id), 1u);
}

TEST_CASE(GaveUpClearsDowngraded) {
    IndexerFixture f;
    auto id = f.workspace.path_pool.intern("/proj/d.cpp");
    f.pump.enqueue(id, ReindexReason::ContentChanged);
    auto launch = f.ticket(id);
    f.set_attempts(id, IndexerFixture::budget);

    // A deps-only enqueue lands mid-flight, then the content pass spends
    // its last life. The downgraded entry must not stay queued: its retry
    // is doomed, and the give-up already accepted the staleness.
    f.consume(id);
    f.pump.enqueue(id, ReindexReason::DepsOnly);
    ASSERT_EQ(int(f.fail_at(id, launch, /*crashed=*/true)), int(IndexerFixture::Verdict::GaveUp));
    ASSERT_FALSE(f.pump.pending_reason(id).has_value());
}

TEST_CASE(DroppedWithoutPending) {
    IndexerFixture f;
    auto id = f.workspace.path_pool.intern("/proj/gone.cpp");
    ASSERT_EQ(int(f.fail(id, /*crashed=*/true)), int(IndexerFixture::Verdict::Dropped));
}

TEST_CASE(AttemptWaitPerTicket) {
    IndexerFixture f;
    auto id = f.workspace.path_pool.intern("/proj/waited.cpp");
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

    // A requeue lands mid-flight: the entry stays pending under a fresh
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
    // as long as edits keep landing.
    f.settle(id, launch);
    ASSERT_TRUE(first_woke);
    ASSERT_FALSE(second_woke);

    f.settle(id, f.ticket(id));
    ASSERT_TRUE(second_woke);
}

TEST_CASE(ContentChangeResetsBudget) {
    IndexerFixture f;
    auto id = f.workspace.path_pool.intern("/proj/fixed.cpp");
    f.pump.enqueue(id, ReindexReason::ContentChanged);

    ASSERT_EQ(int(f.fail(id, /*crashed=*/true)), int(IndexerFixture::Verdict::Requeued));
    ASSERT_EQ(int(f.fail(id, /*crashed=*/true)), int(IndexerFixture::Verdict::Requeued));
    ASSERT_EQ(f.attempts(id), 2u);

    // The user fixes the file: new content starts a fresh poison budget.
    f.pump.enqueue(id, ReindexReason::ContentChanged);
    ASSERT_EQ(f.attempts(id), 0u);

    // A deps-only cascade is not new content and keeps the ledger.
    ASSERT_EQ(int(f.fail(id, /*crashed=*/true)), int(IndexerFixture::Verdict::Requeued));
    f.pump.enqueue(id, ReindexReason::DepsOnly);
    ASSERT_EQ(f.attempts(id), 1u);
}

TEST_CASE(RoundSnapshotBoundary) {
    IndexerFixture f;
    // Manual rounds: the tail schedule() must no-op so the boundary between
    // the two rounds stays observable.
    f.workspace.config.project.enable_indexing.value = false;

    auto a = f.workspace.path_pool.intern("/fake/a.cpp");
    auto b = f.workspace.path_pool.intern("/fake/b.cpp");
    auto c = f.workspace.path_pool.intern("/fake/c.cpp");

    f.pump.enqueue(a, ReindexReason::ContentChanged);
    f.pump.enqueue(b, ReindexReason::ContentChanged);

    // Grow the queue from inside the round: the first Report enqueues a
    // third file, which must land past the round snapshot and wait for the
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

    ASSERT_TRUE(grew);
    ASSERT_EQ(first_end.total, 2u);
    ASSERT_EQ(first_end.dispatched, 2u);
    ASSERT_EQ(first_end.completed, 2u);
    ASSERT_EQ(f.pump.pending_files(), 1u);

    f.run_round();

    ASSERT_EQ(f.pump.pending_files(), 0u);
    ASSERT_EQ(f.pump.failed_files(), 3u);
    ASSERT_TRUE(f.pump.is_idle());
}

TEST_CASE(PauseResumesRound) {
    IndexerFixture f;
    f.workspace.config.project.enable_indexing.value = false;

    auto a = f.workspace.path_pool.intern("/fake/a.cpp");
    auto b = f.workspace.path_pool.intern("/fake/b.cpp");
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

    ASSERT_TRUE(paused);
    ASSERT_EQ(f.pump.pending_files(), 0u);
    ASSERT_EQ(f.pump.failed_files(), 2u);
    ASSERT_TRUE(f.pump.is_idle());
}

};  // TEST_SUITE(IndexerRequeue)

/// The store's neutral change reports and the pump's claim of them — the
/// contracts the Indexer split introduced: every row-changing source
/// reports debt and row changes, the save carries the pump's debt
/// snapshot both ways, and admission is re-judged at landing.
TEST_SUITE(IndexReports) {

TEST_CASE(MergeReportsRowsChanged) {
    IndexerFixture f;
    TempDir tmp;
    tmp.touch("main.cpp", "int value() { return 1; }\n");
    auto indexed = index_file(tmp, tmp.path("main.cpp"));
    ASSERT_FALSE(indexed.data.empty());

    llvm::SmallVector<std::uint32_t> notified;
    auto conn = f.pump.on_rows_changed.connect(
        [&](llvm::ArrayRef<std::uint32_t> ids) { notified.append(ids.begin(), ids.end()); });
    ASSERT_TRUE(f.merge(indexed.data.data(), indexed.data.size()));

    ASSERT_TRUE(llvm::is_contained(notified, f.workspace.path_pool.intern(indexed.tu_path)));
}

TEST_CASE(DropReportsServedRows) {
    // drop_index deletes rows an index-served session may already have
    // consumed; the report must carry every affected file so the serving
    // side can refresh — dropped rows change answers exactly like merged
    // rows do, and no later merge or compile is owed to cover them.
    IndexerFixture f;
    TempDir tmp;
    tmp.touch("dep.h", "#pragma once\ninline int dep() { return 1; }\n");
    tmp.touch("main.cpp", "#include \"dep.h\"\nint use() { return dep(); }\n");
    auto indexed = index_file(tmp, tmp.path("main.cpp"));
    ASSERT_FALSE(indexed.data.empty());
    ASSERT_TRUE(f.merge(indexed.data.data(), indexed.data.size()));

    auto tu_id = f.workspace.path_pool.intern(indexed.tu_path);
    auto header_id = f.workspace.path_pool.intern(tmp.path("dep.h"));

    llvm::SmallVector<std::uint32_t> notified;
    auto conn = f.pump.on_rows_changed.connect(
        [&](llvm::ArrayRef<std::uint32_t> ids) { notified.append(ids.begin(), ids.end()); });
    auto report = f.index_store.drop_index(tu_id);
    ASSERT_TRUE(llvm::is_contained(report.rows_changed(), tu_id));
    ASSERT_TRUE(llvm::is_contained(report.rows_changed(), header_id));

    f.pump.claim_report(report);
    ASSERT_TRUE(llvm::is_contained(notified, header_id));
}

TEST_CASE(RetireReportsRowsChanged) {
    // The save-side recovery source: a shard retired by the compaction
    // vanishes from memory, which changes index-served answers exactly
    // like a merge — the report must say so.
    IndexerFixture f;
    TempDir tmp;
    tmp.touch("dep.h", "#pragma once\ninline int dep() { return 1; }\n");
    tmp.touch("main.cpp", "#include \"dep.h\"\nint use() { return dep(); }\n");
    open_store(tmp, f.workspace);
    auto indexed = index_file(tmp, tmp.path("main.cpp"));
    ASSERT_FALSE(indexed.data.empty());
    ASSERT_TRUE(f.merge(indexed.data.data(), indexed.data.size()));
    f.save();

    // The TU stops including the header: its contribution dies, and the
    // next save retires the header's shard entirely.
    tmp.touch("main.cpp", "int use() { return 0; }\n");
    auto second = index_file(tmp, tmp.path("main.cpp"));
    ASSERT_FALSE(second.data.empty());
    ASSERT_TRUE(f.merge(second.data.data(), second.data.size()));

    auto header_id = f.workspace.path_pool.intern(tmp.path("dep.h"));
    ASSERT_TRUE(f.workspace.shards.contains(header_id));

    llvm::SmallVector<std::uint32_t> notified;
    auto conn = f.pump.on_rows_changed.connect(
        [&](llvm::ArrayRef<std::uint32_t> ids) { notified.append(ids.begin(), ids.end()); });
    f.save();

    ASSERT_FALSE(f.workspace.shards.contains(header_id));
    ASSERT_TRUE(llvm::is_contained(notified, header_id));
}

TEST_CASE(FailedStandaloneInSnapshot) {
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
        open_store(tmp, f.workspace);
        f.workspace.cdb.add_command(tmp.root, src, llvm::StringRef("clang++ -c main.cpp"));
        f.mark_failed(f.workspace.path_pool.intern(header));
        // Something dirty so the save writes at all; the snapshot rides
        // the same batch.
        auto indexed = index_file(tmp, src);
        ASSERT_FALSE(indexed.data.empty());
        ASSERT_TRUE(f.merge(indexed.data.data(), indexed.data.size()));
        f.save();
    }

    IndexerFixture f;
    open_store(tmp, f.workspace);
    f.workspace.cdb.add_command(tmp.root, src, llvm::StringRef("clang++ -c main.cpp"));
    f.load();
    ASSERT_TRUE(f.pump.pending_reason(f.workspace.path_pool.intern(header)) ==
                ReindexReason::ContentChanged);
}

TEST_CASE(LateDebtShutdownRetry) {
    // Debt surfacing after the save serialized its snapshot (write-time
    // corruption recovery) comes back with snapshot_stale set; the
    // shutdown's one metadata retry must land it in the fresh database,
    // or a dropped standalone header's repair debt is lost for good.
    struct CorruptOnWrite final : index::BlobDatabase {
        bool poisoned = false;

        index::ReadBlob read(index::IndexBlobKind, llvm::StringRef) override {
            return {};
        }

        bool contains(index::IndexBlobKind, llvm::StringRef) override {
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

        void for_each_key(index::IndexBlobKind,
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
    open_store(tmp, f.workspace);
    f.workspace.index_db = std::make_unique<CorruptOnWrite>();

    f.mark_failed(f.workspace.path_pool.intern(tmp.path("dep.h")));
    auto indexed = index_file(tmp, tmp.path("main.cpp"));
    ASSERT_FALSE(indexed.data.empty());
    ASSERT_TRUE(f.merge(indexed.data.data(), indexed.data.size()));

    IndexStore::Report report;
    auto body = [&]() -> kota::task<> {
        report = co_await f.index_store.save(f.pump.save_debt());
    };
    auto task = body();
    f.loop.schedule(task);
    f.loop.run();

    ASSERT_TRUE(report.snapshot_stale);
    f.pump.claim_report(report);

    // The retry persists into the freshly reopened database, snapshot
    // included.
    f.save();
    auto blob = f.workspace.index_db->read(index::IndexBlobKind::CDB, "cdb");
    ASSERT_TRUE(bool(blob));
    ASSERT_TRUE(llvm::StringRef(blob.buffer->getBuffer()).contains("dep.h"));
}

TEST_CASE(DispatchDeferKeepsDebt) {
    IndexerFixture f;
    f.workspace.config.project.enable_indexing.value = false;
    auto id = f.workspace.path_pool.intern("/fake/a.cpp");
    f.pump.enqueue(id, ReindexReason::ContentChanged);

    f.pump.admission = [](std::uint32_t) {
        return Admission::Defer;
    };
    f.run_round();

    // The claim was consumed but never settled: the debt stands for a
    // later round, and nothing was counted as failed.
    ASSERT_TRUE(f.pump.pending_reason(id) == ReindexReason::ContentChanged);
    ASSERT_EQ(f.pump.failed_files(), 0u);
    ASSERT_TRUE(f.pump.is_idle());
}

TEST_CASE(LandingVetoDropsResult) {
    // Landing-time admission (the S6 behavior decision): a session
    // arriving while the parse is in flight vetoes the finished result —
    // the merge is dropped and the claim settles, exactly as a
    // dispatch-time veto would have skipped the work.
    IndexerFixture f;
    TempDir tmp;
    tmp.touch("main.cpp", "int value() { return 1; }\n");
    auto src = tmp.path("main.cpp");
    f.workspace.config.project.enable_indexing.value = false;
    f.workspace.cdb.add_command(
        tmp.root,
        src,
        std::format("clang++ -fsyntax-only -resource-dir {} -c {}", resource_dir(), src));

    auto id = f.workspace.path_pool.intern(src);
    f.pump.enqueue(id, ReindexReason::ContentChanged);

    int asks = 0;
    f.pump.admission = [&](std::uint32_t) {
        asks += 1;
        // First ask = dispatch (admit); second = landing, where the
        // serving side has changed its mind.
        return asks == 1 ? Admission::Admit : Admission::SkipAndSettle;
    };

    auto body = [&]() -> kota::task<> {
        WorkerPoolOptions opts;
        opts.self_path = clice_binary();
        opts.stateless_count = 1;
        opts.stateful_count = 0;
        CO_ASSERT_TRUE(f.pool.start(opts));
        co_await f.round_task();
        co_await f.pool.stop();
    };
    auto task = body();
    f.loop.schedule(task);
    f.loop.run();

    ASSERT_EQ(asks, 2);
    ASSERT_FALSE(f.workspace.shards.contains(id));
    ASSERT_FALSE(f.pump.pending_reason(id).has_value());
    ASSERT_EQ(f.pump.failed_files(), 0u);
}

TEST_CASE(BoostRearmsIdleTimer) {
    // s#9: a boost colliding with an already-armed idle timer must re-arm
    // it to fire now. Un-fixed, this test waits out the full idle window
    // below instead of finishing promptly.
    IndexerFixture f;
    f.workspace.config.project.enable_indexing.value = true;
    f.workspace.config.project.idle_timeout_ms.value = 60'000;

    auto id = f.workspace.path_pool.intern("/fake/a.cpp");
    f.pump.enqueue(id, ReindexReason::ContentChanged);
    f.pump.schedule();
    f.pump.boost(id);
    f.loop.run();

    ASSERT_TRUE(f.pump.is_idle());
    ASSERT_EQ(f.pump.failed_files(), 1u);
}

};  // TEST_SUITE(IndexReports)

TEST_SUITE(TURunLint) {

TEST_CASE(ModuleLintScanParity) {
    // A module unit's own PCM round scans under its base command: only
    // the lint round's extras-applied scan can discover an import the
    // extra args gate, and edge it so the PCM exists when the worker's
    // parse (which sees the extras) consumes it. Only n.cppm runs, so
    // the import's PCM cannot arrive any other way.
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
              f.workspace.cdb,
              build_cdb_json({
                  {tmp.root, tmp.path("m.cppm"), {}},
                  {tmp.root, tmp.path("n.cppm"), {}},
    }));
    scan_dependency_graph(f.workspace.cdb, f.workspace.dep_graph);
    f.workspace.dep_graph.build_reverse_map();
    f.workspace.build_module_map();

    auto store = CacheStore::open(tmp.path("root"), 1);
    ASSERT_TRUE(store.has_value());
    store->register_namespace(
        {.name = "pcm", .extension = ".pcm", .policy = CachePolicy::LRU, .max_bytes = 1ull << 30});
    f.workspace.store.emplace(std::move(*store));

    f.pcm.register_runner();

    TURunFamily::Plan plan;
    plan.tidy = true;
    plan.tidy_params.checks = "-*,bugprone-integer-division";
    plan.tidy_params.extra_args = {"-DUSE_M"};

    auto n_id = f.workspace.path_pool.intern(tmp.path("n.cppm"));
    TURunFamily::Outcome outcome;
    bool done = false;
    auto body = [&]() -> kota::task<> {
        WorkerPoolOptions opts;
        opts.self_path = clice_binary();
        opts.stateless_count = 1;
        opts.stateful_count = 0;
        CO_ASSERT_TRUE(f.pool.start(opts));

        outcome = co_await f.turun.run(n_id, std::move(plan), {});

        co_await f.graph.shutdown();
        co_await f.pool.stop();
        done = true;
    };
    auto task = body();
    f.loop.schedule(task);
    f.loop.run();
    EXPECT_TRUE(done);

    EXPECT_TRUE(outcome.verdict == TURunFamily::Verdict::Completed);
    // The finding inside the gated region proves the parse saw the
    // extras and consumed the edge-built PCM.
    ASSERT_FALSE(outcome.tidy_diagnostics.empty());
    EXPECT_EQ(outcome.tidy_diagnostics[0].check, "bugprone-integer-division");
}

};  // TEST_SUITE(TURunLint)

}  // namespace
}  // namespace clice::testing

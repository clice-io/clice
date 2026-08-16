#include <format>
#include <limits>
#include <memory>

#include "test/temp_dir.h"
#include "test/test.h"
#include "command/argument_parser.h"
#include "compile/compilation.h"
#include "index/manifest.h"
#include "index/shard.h"
#include "index/storage.h"
#include "index/tu_index.h"
#include "server/compiler/context_resolver.h"
#include "server/compiler/indexer.h"
#include "server/state/session_store.h"
#include "server/state/workspace.h"
#include "server/worker/worker_pool.h"

#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/xxhash.h"

namespace clice::testing {

/// Test fixture with friend access to Indexer internals.
struct IndexerFixture {
    using Verdict = Indexer::RequeueVerdict;

    constexpr static unsigned budget = Indexer::max_requeue_attempts;

    kota::event_loop loop;
    Workspace workspace;
    WorkerPool pool{loop};
    ContextResolver contexts{workspace};
    SessionStore sessions;
    Indexer indexer{loop, workspace, pool, contexts, sessions};

    /// Fail the entry's current dispatch: the launch ticket matches.
    Verdict fail(std::uint32_t id, bool crashed) {
        return indexer.note_dispatch_failure(id, ticket(id), crashed);
    }

    /// Fail a dispatch launched with an explicit (possibly stale) ticket.
    Verdict fail_at(std::uint32_t id, std::uint64_t ticket, bool crashed) {
        return indexer.note_dispatch_failure(id, ticket, crashed);
    }

    std::uint64_t ticket(std::uint32_t id) {
        auto it = indexer.reindex_reasons.find(id);
        return it == indexer.reindex_reasons.end() ? std::numeric_limits<std::uint64_t>::max()
                                                   : it->second.ticket;
    }

    unsigned attempts(std::uint32_t id) {
        auto it = indexer.reindex_reasons.find(id);
        return it == indexer.reindex_reasons.end() ? 0u : it->second.requeue_attempts;
    }

    void set_attempts(std::uint32_t id, unsigned n) {
        indexer.reindex_reasons.find(id)->second.requeue_attempts = n;
    }

    /// Consume the queued slot as a dispatch would, so a later enqueue
    /// takes the fresh-slot (mid-flight) path.
    void consume(std::uint32_t id) {
        indexer.pending_ids.erase(id);
    }

    /// Start a fresh staleness round: per-round FileVersion verdicts are
    /// cleared by run_background_indexing, which these tests bypass.
    void clear_verdicts() {
        indexer.fv_verdicts.clear();
    }

    /// Judge staleness inside the current round (see clear_verdicts).
    bool need_update(llvm::StringRef path) {
        return indexer.need_update(path);
    }

    bool global_dirty() {
        return indexer.global_dirty;
    }

    /// Drop the merge's own dirty mark so a later assertion isolates the
    /// stamp-repair path.
    void reset_global_dirty() {
        indexer.global_dirty = false;
    }

    /// Run one save() to completion on the fixture's loop.
    void save() {
        auto body = [this]() -> kota::task<> {
            co_await indexer.save();
        };
        auto task = body();
        loop.schedule(task);
        loop.run();
    }
};

namespace {

struct IndexedTU {
    std::string data;     ///< Serialized TUIndex, as a worker would ship it.
    std::string tu_path;  ///< The TU's canonical path inside the index.
};

/// Index a real on-disk file in-process and serialize its TUIndex.
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
    auto tu_index = index::TUIndex::build(unit);
    IndexedTU result;
    result.tu_path = tu_index.graph.paths.back();
    llvm::raw_string_ostream os(result.data);
    tu_index.serialize(os);
    return result;
}

void open_store(TempDir& tmp, Workspace& workspace) {
    auto store = CacheStore::open(tmp.path("cache"), 1);
    ASSERT_TRUE(store.has_value());
    workspace.store.emplace(std::move(*store));
    workspace.index_storage = index::make_fs_index_storage(*workspace.store);
}

/// The storage key of a file's shard or manifest blob (Indexer's naming).
std::string blob_key(llvm::StringRef path) {
    return std::format("{:016x}", llvm::xxh3_64bits(path));
}

TEST_SUITE(IndexerMerge) {

kota::event_loop loop;
Workspace workspace;
SessionStore store;
WorkerPool pool{loop};
ContextResolver resolver{workspace};
Indexer indexer{loop, workspace, pool, resolver, store};

TEST_CASE(MergeRejectsGarbage) {
    // A worker shipping corrupted bytes (torn write, stale format) must not
    // crash the master or leave partial state behind.
    ASSERT_TRUE(workspace.shards.empty());
    ASSERT_TRUE(workspace.project_index.symbols.empty());

    std::string garbage = "definitely not a flatbuffer, but long enough to try";
    indexer.merge(garbage.data(), garbage.size());

    ASSERT_TRUE(workspace.shards.empty());
    ASSERT_TRUE(workspace.project_index.symbols.empty());
}

TEST_CASE(MergeSkipsMovedDisk) {
    TempDir tmp;
    tmp.touch("main.cpp", "int value() { return 1; }\n");
    auto src = tmp.path("main.cpp");

    auto indexed = index_file(tmp, src);
    ASSERT_FALSE(indexed.data.empty());

    indexer.merge(indexed.data.data(), indexed.data.size());
    auto path_id = workspace.path_pool.intern(indexed.tu_path);
    auto it = workspace.shards.find(path_id);
    ASSERT_TRUE(it != workspace.shards.end());
    ASSERT_EQ(it->second.content(), "int value() { return 1; }\n");

    // The disk moved on since the rows were indexed: merging them would
    // pair offsets with bytes they were not built from, so the merge must
    // be skipped and the last-known snapshot kept serving.
    tmp.touch("main.cpp", "int renamed() { return 2; }\n");
    indexer.merge(indexed.data.data(), indexed.data.size());
    ASSERT_EQ(it->second.content(), "int value() { return 1; }\n");
    ASSERT_TRUE(workspace.project_index.contributions.lookup(path_id).contains(path_id));

    // Once the rows describe the settled content again, the merge lands.
    auto fresh = index_file(tmp, src);
    ASSERT_FALSE(fresh.data.empty());
    indexer.merge(fresh.data.data(), fresh.data.size());
    ASSERT_EQ(it->second.content(), "int renamed() { return 2; }\n");
}

TEST_CASE(SaveCommitsDirtyShard) {
    TempDir tmp;
    tmp.touch("main.cpp", "int flip_value() { return 1; }\n");
    auto src = tmp.path("main.cpp");
    open_store(tmp, workspace);

    auto indexed = index_file(tmp, src);
    ASSERT_FALSE(indexed.data.empty());
    indexer.merge(indexed.data.data(), indexed.data.size());

    auto path_id = workspace.path_pool.intern(indexed.tu_path);
    ASSERT_EQ(indexer.pending_shard_writes(), 1u);

    // Named body: a temporary lambda's captures die with the statement
    // while the coroutine frame still references them.
    auto save_body = [&]() -> kota::task<> {
        co_await indexer.save();
    };
    auto task = save_body();
    loop.schedule(task);
    loop.run();

    // Committed: the dirty state is drained and the shard still answers
    // identically.
    auto it = workspace.shards.find(path_id);
    ASSERT_TRUE(it != workspace.shards.end());
    ASSERT_EQ(indexer.pending_shard_writes(), 0u);
    ASSERT_EQ(indexer.last_save_shards(), 1u);
    ASSERT_EQ(it->second.content(), "int flip_value() { return 1; }\n");
    ASSERT_TRUE(workspace.project_index.contributions.lookup(path_id).contains(path_id));
}

TEST_CASE(MidSaveMergeKept) {
    TempDir tmp;
    tmp.touch("main.cpp", "int first_value() { return 1; }\n");
    auto src = tmp.path("main.cpp");
    open_store(tmp, workspace);

    auto indexed = index_file(tmp, src);
    ASSERT_FALSE(indexed.data.empty());
    indexer.merge(indexed.data.data(), indexed.data.size());
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
        co_await indexer.save();
    };
    std::size_t mid_save_pending = 0;
    auto merge_body = [&]() -> kota::task<> {
        mid_save_pending = indexer.pending_shard_writes();
        indexer.merge(fresh.data.data(), fresh.data.size());
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
    ASSERT_EQ(indexer.pending_shard_writes(), 1u);
    ASSERT_EQ(it->second.content(), "int second_value() { return 2; }\n");

    auto again_body = [&]() -> kota::task<> {
        co_await indexer.save();
    };
    auto task = again_body();
    loop.schedule(task);
    loop.run();

    it = workspace.shards.find(path_id);
    ASSERT_EQ(indexer.pending_shard_writes(), 0u);
    ASSERT_EQ(it->second.content(), "int second_value() { return 2; }\n");
}

TEST_CASE(MergeHitWritesNothing) {
    TempDir tmp;
    tmp.touch("main.cpp", "int steady() { return 1; }\n");
    auto src = tmp.path("main.cpp");
    open_store(tmp, workspace);

    auto indexed = index_file(tmp, src);
    ASSERT_FALSE(indexed.data.empty());
    indexer.merge(indexed.data.data(), indexed.data.size());
    auto save_body = [&]() -> kota::task<> {
        co_await indexer.save();
    };
    auto task = save_body();
    loop.schedule(task);
    loop.run();
    ASSERT_EQ(indexer.pending_shard_writes(), 0u);

    // A re-merge whose rows the shard already stores is the steady state of
    // every background round: it must record contributions and touch no
    // blob at all.
    indexer.merge(indexed.data.data(), indexed.data.size());
    ASSERT_EQ(indexer.pending_shard_writes(), 0u);
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
    indexer.merge(a.data.data(), a.data.size());
    indexer.merge(b.data.data(), b.data.size());
    auto header_id = workspace.path_pool.intern(tmp.path("shared.h"));
    auto& shard = workspace.shards[header_id];
    ASSERT_EQ(shard.variants().size(), std::size_t(2));
    ASSERT_EQ(workspace.project_index.contributions.lookup(header_id).size(), std::size_t(2));

    // A third TU sharing a's preprocessing hits the stored variant: the
    // set does not grow, and neither existing contribution is disturbed.
    tmp.touch("c.cpp", "#include \"shared.h\"\nint c() { return shared_fn(); }\n");
    auto c = index_file(tmp, tmp.path("c.cpp"));
    ASSERT_FALSE(c.data.empty());
    indexer.merge(c.data.data(), c.data.size());
    ASSERT_EQ(shard.variants().size(), std::size_t(2));
    ASSERT_EQ(workspace.project_index.contributions.lookup(header_id).size(), std::size_t(3));

    // Re-indexing a TU whose header rows are unchanged must not disturb
    // the other TUs' variants either.
    tmp.touch("a.cpp", "#include \"shared.h\"\nint a2() { return shared_fn(); }\n");
    auto fresh = index_file(tmp, tmp.path("a.cpp"));
    ASSERT_FALSE(fresh.data.empty());
    indexer.merge(fresh.data.data(), fresh.data.size());
    ASSERT_EQ(shard.variants().size(), std::size_t(2));
    ASSERT_EQ(workspace.project_index.contributions.lookup(header_id).size(), std::size_t(3));
}

TEST_CASE(HeaderSkipCarriesContribution) {
    TempDir tmp;
    tmp.touch("dep.h", "#pragma once\ninline int dep() { return 1; }\n");
    tmp.touch("main.cpp", "#include \"dep.h\"\nint use() { return dep(); }\n");
    auto src = tmp.path("main.cpp");

    auto v1 = index_file(tmp, src);
    ASSERT_FALSE(v1.data.empty());
    indexer.merge(v1.data.data(), v1.data.size());
    auto header_id = workspace.path_pool.intern(tmp.path("dep.h"));
    auto tu_id = workspace.path_pool.intern(v1.tu_path);
    auto old_hash = workspace.project_index.contributions.lookup(header_id).lookup(tu_id);
    ASSERT_TRUE(old_hash != 0);

    // The header changes, a reindex captures it — and the header changes
    // AGAIN before the result merges. The stale section must not land, but
    // the previous contribution keeps serving (its rows still match the
    // shard) until a follow-up pass settles.
    tmp.touch("dep.h", "#pragma once\ninline int dep() { return 2; }\n");
    auto v2 = index_file(tmp, src);
    ASSERT_FALSE(v2.data.empty());
    tmp.touch("dep.h", "#pragma once\ninline int dep() { return 3; }\n");

    indexer.merge(v2.data.data(), v2.data.size());
    ASSERT_EQ(workspace.project_index.contributions.lookup(header_id).lookup(tu_id), old_hash);
    ASSERT_TRUE(workspace.shards[header_id].has_variant(old_hash));
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
    indexer.merge(a.data.data(), a.data.size());
    indexer.merge(b.data.data(), b.data.size());
    auto header_id = workspace.path_pool.intern(tmp.path("shared.h"));
    ASSERT_EQ(workspace.shards[header_id].variants().size(), std::size_t(2));

    auto save = [&] {
        auto body = [&]() -> kota::task<> {
            co_await indexer.save();
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
    indexer.merge(b2.data.data(), b2.data.size());
    ASSERT_TRUE(workspace.shards[header_id].has_dead_variants());
    save();
    ASSERT_EQ(workspace.shards[header_id].variants().size(), std::size_t(1));

    // a drops it too: no contribution is left, so the shard retires from
    // memory and from storage — with no owner left to re-enqueue.
    tmp.touch("a.cpp", "int a() { return 3; }\n");
    auto a2 = index_file(tmp, tmp.path("a.cpp"));
    ASSERT_FALSE(a2.data.empty());
    indexer.merge(a2.data.data(), a2.data.size());
    save();
    ASSERT_FALSE(workspace.shards.contains(header_id));
    ASSERT_FALSE(indexer.pending_reason(workspace.path_pool.intern(a2.tu_path)).has_value());
    bool on_disk = false;
    auto key = blob_key(workspace.path_pool.resolve(header_id));
    workspace.index_storage->for_each_key(index::IndexBlobKind::Shard,
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
    indexer.merge(a.data.data(), a.data.size());
    indexer.merge(b.data.data(), b.data.size());
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
    indexer.merge(a2.data.data(), a2.data.size());
    ASSERT_EQ(workspace.shards[header_id].variants().size(), std::size_t(1));

    // pa's index drops before pb reindexes: every stored variant is dead,
    // but pb's pinned hash keeps the live set nonempty. The save must
    // retire the shard rather than compact to an empty variant set.
    indexer.drop_index(workspace.path_pool.intern(a2.tu_path));
    auto body = [&]() -> kota::task<> {
        co_await indexer.save();
    };
    auto task = body();
    loop.schedule(task);
    loop.run();

    ASSERT_FALSE(workspace.shards.contains(header_id));
    bool on_disk = false;
    auto key = blob_key(workspace.path_pool.resolve(header_id));
    workspace.index_storage->for_each_key(index::IndexBlobKind::Shard,
                                          [&](llvm::StringRef k) { on_disk |= k == key; });
    ASSERT_FALSE(on_disk);

    // pb's manifest survives, still pinning rows the retirement made
    // unservable; nothing else in this process would rebuild them (a
    // reverted header even reads fresh by hash), so the retirement must
    // re-enqueue pb itself.
    ASSERT_TRUE(indexer.pending_reason(workspace.path_pool.intern(b.tu_path)) ==
                ReindexReason::ContentChanged);
}

TEST_CASE(RejectsCorruptSection) {
    TempDir tmp;
    tmp.touch("cor.h", "#pragma once\ninline int cor() { return 1; }\n");
    tmp.touch("cor_main.cpp", "#include \"cor.h\"\nint use_cor() { return cor(); }\n");

    auto indexed = index_file(tmp, tmp.path("cor_main.cpp"));
    ASSERT_FALSE(indexed.data.empty());

    // Corrupt the main file's nested rows section: the outer wire still
    // verifies (sections are opaque bytes to it), only the nested decode
    // fails.
    auto tampered = index::TUIndex::from(indexed.data);
    ASSERT_TRUE(tampered.has_value());
    auto main_id = static_cast<std::uint32_t>(tampered->graph.paths.size() - 1);
    for(auto& section: tampered->sections) {
        if(section.path_id == main_id) {
            section.rows = {0, 1, 2, 3};
        }
    }
    std::string corrupt;
    llvm::raw_string_ostream os(corrupt);
    tampered->serialize(os);

    // The header section decodes fine and is staged before the main
    // section's decode fails; the reject must discard the whole result — a
    // manifest whose recorded versions all match the disk would otherwise
    // be judged fresh forever with the main file's rows missing.
    indexer.merge(corrupt.data(), corrupt.size());
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
    indexer.merge(indexed.data.data(), indexed.data.size());
    ASSERT_TRUE(workspace.project_index.manifests.contains(tu_id));
    ASSERT_TRUE(workspace.shards.contains(header_id));
}

TEST_CASE(UnverifiedPairingSkipped) {
    TempDir tmp;
    tmp.touch("unv.cpp", "int unverified_fn() { return 123456; }\n");
    auto src = tmp.path("unv.cpp");
    auto indexed = index_file(tmp, src);
    ASSERT_FALSE(indexed.data.empty());

    // Strip the consumed-content hashes (a file behind a PCM ships none)
    // and shrink the file: the content arbitration cannot see the disk
    // moving on, but the rows overrun the shorter content and the fresh
    // blob's own range bounds must catch it — a skip, not a blob serving
    // ranges past its content.
    auto tampered = index::TUIndex::from(indexed.data);
    ASSERT_TRUE(tampered.has_value());
    for(auto& hash: tampered->graph.path_hashes) {
        hash = 0;
    }
    std::string wire;
    llvm::raw_string_ostream os(wire);
    tampered->serialize(os);
    tmp.touch("unv.cpp", "int f;\n");

    indexer.merge(wire.data(), wire.size());
    ASSERT_FALSE(workspace.shards.contains(workspace.path_pool.intern(src)));
}

TEST_CASE(HashlessRemergeHits) {
    TempDir tmp;
    tmp.touch("pcm.cpp", "int hashless_fn() { return 7; }\n");
    auto src = tmp.path("pcm.cpp");
    auto indexed = index_file(tmp, src);
    ASSERT_FALSE(indexed.data.empty());

    // A file behind a PCM ships no consumed-content hash, so the no-IO
    // fast path cannot vouch for a stored variant; only the disk read can.
    auto tampered = index::TUIndex::from(indexed.data);
    ASSERT_TRUE(tampered.has_value());
    for(auto& hash: tampered->graph.path_hashes) {
        hash = 0;
    }
    std::string wire;
    llvm::raw_string_ostream os(wire);
    tampered->serialize(os);

    indexer.merge(wire.data(), wire.size());
    auto path_id = workspace.path_pool.intern(src);
    ASSERT_EQ(workspace.shards[path_id].variants().size(), std::size_t(1));

    // Re-merging the same rows must register as a hit, not append the
    // stored variant to the blob a second time.
    indexer.merge(wire.data(), wire.size());
    ASSERT_EQ(workspace.shards[path_id].variants().size(), std::size_t(1));
}

TEST_CASE(FailedWriteNotCounted) {
    TempDir tmp;
    tmp.touch("main.cpp", "int uncommitted() { return 1; }\n");
    auto src = tmp.path("main.cpp");

    // A storage whose commits never land (disk full, permissions): the
    // gauge must report what was durably committed, not what the save
    // attempted.
    struct FailingStorage final : index::IndexStorage {
        std::unique_ptr<llvm::MemoryBuffer> read(index::IndexBlobKind, llvm::StringRef) override {
            return nullptr;
        }

        bool contains(index::IndexBlobKind, llvm::StringRef) override {
            return false;
        }

        llvm::SmallVector<std::size_t> write(llvm::ArrayRef<Blob> batch) override {
            llvm::SmallVector<std::size_t> failed;
            for(std::size_t i = 0; i < batch.size(); i += 1) {
                failed.push_back(i);
            }
            return failed;
        }

        void remove(index::IndexBlobKind, llvm::StringRef) override {}

        void for_each_key(index::IndexBlobKind,
                          llvm::function_ref<void(llvm::StringRef)>) override {}
    };

    workspace.index_storage = std::make_unique<FailingStorage>();

    auto indexed = index_file(tmp, src);
    ASSERT_FALSE(indexed.data.empty());
    indexer.merge(indexed.data.data(), indexed.data.size());
    ASSERT_EQ(indexer.pending_shard_writes(), 1u);

    auto save = [&] {
        auto body = [&]() -> kota::task<> {
            co_await indexer.save();
        };
        auto task = body();
        loop.schedule(task);
        loop.run();
    };
    save();
    ASSERT_EQ(indexer.last_save_shards(), 0u);
    // The failed batch is re-dirtied rather than discarded, so a later
    // save has it to retry and the cache converges once the storage
    // recovers.
    ASSERT_EQ(indexer.pending_shard_writes(), 1u);

    open_store(tmp, workspace);
    save();
    ASSERT_EQ(indexer.last_save_shards(), 1u);
    ASSERT_EQ(indexer.pending_shard_writes(), 0u);
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
        f.indexer.merge(indexed.data.data(), indexed.data.size());
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
    f.indexer.merge(indexed.data.data(), indexed.data.size());
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
        f.indexer.merge(indexed.data.data(), indexed.data.size());
        f.save();
    }

    IndexerFixture f;
    open_store(tmp, f.workspace);
    f.indexer.load();

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
        f.indexer.merge(indexed.data.data(), indexed.data.size());
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
    f.indexer.load();

    // The header's rows are unservable, so its contributing TU's manifest
    // is dropped and the TU re-enqueued — no CDB entry would ever re-index
    // a header otherwise. The orphan is swept.
    auto tu_id = f.workspace.path_pool.intern(src);
    ASSERT_TRUE(f.workspace.project_index.manifests.empty());
    ASSERT_TRUE(f.indexer.pending_reason(tu_id).has_value());

    // The dropped manifest also retired the TU's contribution to the
    // OTHER header: its loaded shard's live mask must follow, or it keeps
    // serving a variant nothing contributes any more.
    auto extra_id = f.workspace.path_pool.intern(tmp.path("extra.h"));
    auto extra_it = f.workspace.shards.find(extra_id);
    ASSERT_TRUE(extra_it != f.workspace.shards.end());
    ASSERT_TRUE(extra_it->second.has_dead_variants());
    bool orphan_alive = false;
    f.workspace.index_storage->for_each_key(index::IndexBlobKind::Shard, [&](llvm::StringRef key) {
        orphan_alive |= key == "deadbeefdeadbeef";
    });
    ASSERT_FALSE(orphan_alive);
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
        f.indexer.merge(indexed.data.data(), indexed.data.size());
        f.save();
        header_key = blob_key(
            f.workspace.path_pool.resolve(f.workspace.path_pool.intern(tmp.path("dep.h"))));
    }

    // Replace the header's blob with one that verifies but stores a variant
    // no manifest contributed — the residue of a crash or failed write that
    // landed the manifest without its shard.
    index::FileIndex no_rows;
    index::VariantInput stranger{.hash = 0x1234, .rows = &no_rows};
    std::string bytes;
    llvm::raw_string_ostream os(bytes);
    index::write_shard({}, {}, stranger, "", 0, os);
    tmp.touch("cache/cache/v1/index/" + header_key + ".idx", bytes);

    IndexerFixture f;
    open_store(tmp, f.workspace);
    f.indexer.load();

    // set_live would silently drop the missing rows, so the shard is as
    // unservable as an unreadable one: the TU's manifest goes and the TU
    // re-enqueues.
    auto tu_id = f.workspace.path_pool.intern(src);
    ASSERT_TRUE(f.workspace.project_index.manifests.empty());
    ASSERT_TRUE(f.indexer.pending_reason(tu_id).has_value());
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
        f.indexer.merge(indexed.data.data(), indexed.data.size());
        f.save();
        auto header_id = f.workspace.path_pool.intern(tmp.path("dep.h"));
        auto tu_id = f.workspace.path_pool.intern(src);
        rows_hash = f.workspace.project_index.contributions.lookup(header_id).lookup(tu_id);
        ASSERT_TRUE(rows_hash != 0);
        header_key = blob_key(f.workspace.path_pool.resolve(header_id));
    }

    // Replace the header's blob with one from ANOTHER content generation
    // that still stores the contributed variant — the residue of a failed
    // shard write when an edit past every indexed row keeps the rows hash
    // identical. Every recorded FileVersion matches the disk, so only the
    // generation pin can tell that positions would map through stale text.
    index::FileIndex no_rows;
    index::VariantInput same_rows{.hash = rows_hash, .rows = &no_rows};
    std::string bytes;
    llvm::raw_string_ostream os(bytes);
    index::write_shard({}, {}, same_rows, "stale text", llvm::xxh3_64bits("stale text"), os);
    tmp.touch("cache/cache/v1/index/" + header_key + ".idx", bytes);

    IndexerFixture f;
    open_store(tmp, f.workspace);
    f.indexer.load();

    auto tu_id = f.workspace.path_pool.intern(src);
    ASSERT_TRUE(f.workspace.project_index.manifests.empty());
    ASSERT_TRUE(f.indexer.pending_reason(tu_id).has_value());
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
        f.indexer.merge(indexed.data.data(), indexed.data.size());
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
        f.workspace.index_storage->write({
            {index::IndexBlobKind::Manifest,
             blob_key(f.workspace.path_pool.resolve(tu_id)),
             std::move(bytes)}
        });
    }

    IndexerFixture f;
    open_store(tmp, f.workspace);
    f.indexer.load();

    // The raced manifest is dropped and its TU re-enqueued; the reindex
    // rewrites the manifest and the global together.
    auto tu_id = f.workspace.path_pool.intern(src);
    ASSERT_TRUE(f.workspace.project_index.manifests.empty());
    ASSERT_TRUE(f.indexer.pending_reason(tu_id) == ReindexReason::ContentChanged);
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
        f.indexer.merge(indexed.data.data(), indexed.data.size());
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
        f.workspace.index_storage->write({
            {index::IndexBlobKind::Manifest,
             blob_key(f.workspace.path_pool.resolve(tu_id)),
             std::move(bytes)}
        });
    }

    IndexerFixture f;
    open_store(tmp, f.workspace);
    f.indexer.load();

    // The mistamped manifest is dropped and the TU re-enqueued instead of
    // the previous reindex's dependency set and rows serving as current.
    auto tu_id = f.workspace.path_pool.intern(src);
    ASSERT_TRUE(f.workspace.project_index.manifests.empty());
    ASSERT_TRUE(f.indexer.pending_reason(tu_id) == ReindexReason::ContentChanged);
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
        f.indexer.merge(indexed.data.data(), indexed.data.size());
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
        f.workspace.index_storage->write({
            {index::IndexBlobKind::Manifest, blob_key(header), std::move(bytes)}
        });
    }

    IndexerFixture f;
    open_store(tmp, f.workspace);
    f.indexer.load();

    // The unresolvable manifest is dropped from storage and its TU
    // re-enqueued instead of losing its persisted index forever.
    auto header_id = f.workspace.path_pool.intern(header);
    ASSERT_TRUE(f.indexer.pending_reason(header_id) == ReindexReason::ContentChanged);
    ASSERT_FALSE(f.workspace.project_index.manifests.contains(header_id));
    bool stale_alive = false;
    f.workspace.index_storage->for_each_key(
        index::IndexBlobKind::Manifest,
        [&](llvm::StringRef key) { stale_alive |= key == blob_key(header); });
    ASSERT_FALSE(stale_alive);

    // The TU whose manifest resolved is untouched.
    auto tu_id = f.workspace.path_pool.intern(src);
    ASSERT_TRUE(f.workspace.project_index.manifests.contains(tu_id));
    ASSERT_FALSE(f.indexer.pending_reason(tu_id).has_value());
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
        f.indexer.merge(indexed.data.data(), indexed.data.size());
        f.save();
    }

    // The global blob exists but fails to open — a transient IO error at
    // startup, not absence. Sweeping would destroy the intact index; a
    // fresh lineage saved over the unread one could alias its fv ids and
    // generation stamps. The session must run memory-only and leave every
    // blob for the next start.
    struct UnreadableGlobal final : index::IndexStorage {
        std::unique_ptr<index::IndexStorage> real;

        std::unique_ptr<llvm::MemoryBuffer> read(index::IndexBlobKind kind,
                                                 llvm::StringRef key) override {
            return kind == index::IndexBlobKind::Global ? nullptr : real->read(kind, key);
        }

        bool contains(index::IndexBlobKind kind, llvm::StringRef key) override {
            return real->contains(kind, key);
        }

        llvm::SmallVector<std::size_t> write(llvm::ArrayRef<Blob> batch) override {
            return real->write(batch);
        }

        void remove(index::IndexBlobKind kind, llvm::StringRef key) override {
            real->remove(kind, key);
        }

        void for_each_key(index::IndexBlobKind kind,
                          llvm::function_ref<void(llvm::StringRef)> fn) override {
            real->for_each_key(kind, fn);
        }
    };

    {
        IndexerFixture f;
        open_store(tmp, f.workspace);
        auto wrapper = std::make_unique<UnreadableGlobal>();
        wrapper->real = std::move(f.workspace.index_storage);
        f.workspace.index_storage = std::move(wrapper);
        f.indexer.load();
        ASSERT_TRUE(f.workspace.project_index.manifests.empty());
        ASSERT_TRUE(f.workspace.index_storage == nullptr);
    }

    IndexerFixture f;
    open_store(tmp, f.workspace);
    f.indexer.load();
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
        f.indexer.merge(indexed.data.data(), indexed.data.size());
        f.save();

        // The compile command changed: content freshness cannot see it, so
        // the TU's index is dropped wholesale and staleness flips at once.
        f.indexer.drop_index(f.workspace.path_pool.intern(src));
        ASSERT_TRUE(f.workspace.project_index.manifests.empty());
        ASSERT_TRUE(f.need_update(src));
        f.save();
    }

    // The drop survives a restart: nothing on disk resurrects the
    // old-command rows as fresh.
    IndexerFixture f;
    open_store(tmp, f.workspace);
    f.indexer.load();
    ASSERT_TRUE(f.workspace.project_index.manifests.empty());
    ASSERT_TRUE(f.workspace.shards.empty());
    ASSERT_TRUE(f.need_update(src));
}

};  // TEST_SUITE(IndexerLoad)

TEST_SUITE(IndexerRequeue) {

TEST_CASE(PreemptionKeepsBudget) {
    IndexerFixture f;
    auto id = f.workspace.path_pool.intern("/proj/a.cpp");
    f.indexer.enqueue(id, ReindexReason::ContentChanged);

    // A preemption under memory pressure requeues without spending the
    // crash budget, no matter how often it repeats.
    for(unsigned i = 0; i < 2 * IndexerFixture::budget; ++i) {
        ASSERT_EQ(int(f.fail(id, /*crashed=*/false)), int(IndexerFixture::Verdict::Requeued));
    }
    ASSERT_EQ(f.attempts(id), 0u);
    ASSERT_TRUE(f.indexer.pending_reason(id).has_value());
}

TEST_CASE(CrashSpendsBudget) {
    IndexerFixture f;
    auto id = f.workspace.path_pool.intern("/proj/poison.cpp");
    f.indexer.enqueue(id, ReindexReason::ContentChanged);

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
    ASSERT_FALSE(f.indexer.pending_reason(id).has_value());
    ASSERT_EQ(int(f.fail(id, /*crashed=*/true)), int(IndexerFixture::Verdict::Dropped));
}

TEST_CASE(StaleCrashKeepsBudget) {
    IndexerFixture f;
    auto id = f.workspace.path_pool.intern("/proj/edited.cpp");
    f.indexer.enqueue(id, ReindexReason::ContentChanged);
    auto stale = f.ticket(id);

    // The user fixes the file while the old bytes' dispatch is in flight:
    // the stale crash must not spend the fixed content's budget or touch
    // its pending slot.
    f.indexer.enqueue(id, ReindexReason::ContentChanged);
    ASSERT_EQ(int(f.fail_at(id, stale, /*crashed=*/true)),
              int(IndexerFixture::Verdict::Superseded));
    ASSERT_EQ(f.attempts(id), 0u);
    ASSERT_TRUE(f.indexer.pending_reason(id).has_value());
}

TEST_CASE(DepsDowngradeKeepsDebt) {
    IndexerFixture f;
    auto id = f.workspace.path_pool.intern("/proj/c.cpp");
    f.indexer.enqueue(id, ReindexReason::ContentChanged);
    auto launch = f.ticket(id);

    // The content pass is dispatched; a deps-only cascade lands mid-flight
    // and downgrades the pending reason, betting on that pass to cover the
    // edit. The pass fails — the requeue must restore the ContentChanged
    // debt or the stale shard stops being suppressed.
    f.consume(id);
    f.indexer.enqueue(id, ReindexReason::DepsOnly);
    ASSERT_EQ(int(*f.indexer.pending_reason(id)), int(ReindexReason::DepsOnly));

    ASSERT_EQ(int(f.fail_at(id, launch, /*crashed=*/true)), int(IndexerFixture::Verdict::Requeued));
    ASSERT_EQ(int(*f.indexer.pending_reason(id)), int(ReindexReason::ContentChanged));
    ASSERT_EQ(f.attempts(id), 1u);
}

TEST_CASE(GaveUpClearsDowngraded) {
    IndexerFixture f;
    auto id = f.workspace.path_pool.intern("/proj/d.cpp");
    f.indexer.enqueue(id, ReindexReason::ContentChanged);
    auto launch = f.ticket(id);
    f.set_attempts(id, IndexerFixture::budget);

    // A deps-only enqueue lands mid-flight, then the content pass spends
    // its last life. The downgraded entry must not stay queued: its retry
    // is doomed, and the give-up already accepted the staleness.
    f.consume(id);
    f.indexer.enqueue(id, ReindexReason::DepsOnly);
    ASSERT_EQ(int(f.fail_at(id, launch, /*crashed=*/true)), int(IndexerFixture::Verdict::GaveUp));
    ASSERT_FALSE(f.indexer.pending_reason(id).has_value());
}

TEST_CASE(DroppedWithoutPending) {
    IndexerFixture f;
    auto id = f.workspace.path_pool.intern("/proj/gone.cpp");
    ASSERT_EQ(int(f.fail(id, /*crashed=*/true)), int(IndexerFixture::Verdict::Dropped));
}

TEST_CASE(ContentChangeResetsBudget) {
    IndexerFixture f;
    auto id = f.workspace.path_pool.intern("/proj/fixed.cpp");
    f.indexer.enqueue(id, ReindexReason::ContentChanged);

    ASSERT_EQ(int(f.fail(id, /*crashed=*/true)), int(IndexerFixture::Verdict::Requeued));
    ASSERT_EQ(int(f.fail(id, /*crashed=*/true)), int(IndexerFixture::Verdict::Requeued));
    ASSERT_EQ(f.attempts(id), 2u);

    // The user fixes the file: new content starts a fresh poison budget.
    f.indexer.enqueue(id, ReindexReason::ContentChanged);
    ASSERT_EQ(f.attempts(id), 0u);

    // A deps-only cascade is not new content and keeps the ledger.
    ASSERT_EQ(int(f.fail(id, /*crashed=*/true)), int(IndexerFixture::Verdict::Requeued));
    f.indexer.enqueue(id, ReindexReason::DepsOnly);
    ASSERT_EQ(f.attempts(id), 1u);
}

};  // TEST_SUITE(IndexerRequeue)

}  // namespace
}  // namespace clice::testing

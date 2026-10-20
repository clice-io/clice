module;

#include "modules/prelude.h"

module clice;

import :index.database;
import :index.format;
import :index.project_index;
import :index.tu_manifest;
import :project.project;
import :sched.index.pending_table;
import :support.bytes;
import :unittests.utils.indexer_fixture;
import :unittests.utils.temp_dir;
import :unittests.utils;
import :vfs.cache_store;
import :vfs.ids;
import :vfs.path;

namespace clice::testing {
namespace {

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
    return std::string(as_chars(index::encode(blob)));
}

ZEST_SUITE(ProjectIndexLoad) {

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
        f.merge(indexed.data);
        f.save();
    }

    IndexerFixture f;
    open_store(tmp, f.project);
    f.load();

    auto tu_id = f.project.file_table.intern(Spelling::absolute(src));
    auto header_id = f.project.file_table.intern(Spelling::absolute(tmp.path("dep.h")));
    ZASSERT(f.project.index.shard(tu_id) != nullptr);
    ZASSERT(f.project.index.shard(header_id) != nullptr);
    ZASSERT(contributions_of(f.project.index, header_id).contains(tu_id));
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
        f.merge(indexed.data);
        // The plain save persists the table; the idle one then rebuilds
        // the search index with no other change to write.
        f.save();
        ZASSERT(!f.project.index.has_unsaved_state());
        ZASSERT(!f.project.index.search_index().loaded());
        f.save(index::SearchRebuild::Idle);
        ZASSERT(f.project.index.search_index().loaded());
    }

    IndexerFixture f;
    open_store(tmp, f.project);
    f.load();
    ZASSERT(f.project.index.search_index().loaded());
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
        f.merge(indexed.data);
        f.save();
        header_key = index::blob_key(f.project.file_table.resolve(
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
    ZASSERT(f.project.index.manifest_count() == 0);
    ZASSERT(f.pump.pending_reason(tu_id));

    // The dropped manifest also retired the TU's contribution to the
    // OTHER header: its loaded shard's live mask must follow, or it keeps
    // serving a variant nothing contributes any more.
    auto extra_id = f.project.file_table.intern(Spelling::absolute(tmp.path("extra.h")));
    auto* extra_it = f.project.index.shard(extra_id);
    ZASSERT(extra_it);
    ZASSERT(extra_it->has_dead_variants());

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
        f.merge(indexed.data);
        f.save();
        header_key = index::blob_key(f.project.file_table.resolve(
            f.project.file_table.intern(Spelling::absolute(tmp.path("dep.h")))));
        manifest_key = index::blob_key(
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
    ZASSERT(f.project.index.manifest_count() == 0);
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
        f.merge(indexed.data);
        f.save();
        header_key = index::blob_key(f.project.file_table.resolve(
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
    ZASSERT(f.project.index.manifest_count() == 0);
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
        f.merge(indexed.data);
        f.save();
        auto header_id = f.project.file_table.intern(Spelling::absolute(tmp.path("dep.h")));
        auto tu_id = f.project.file_table.intern(Spelling::absolute(src));
        variant = contributions_of(f.project.index, header_id).lookup(tu_id);
        ZASSERT(variant != 0);
        header_key = index::blob_key(f.project.file_table.resolve(header_id));
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
    ZASSERT(f.project.index.manifest_count() == 0);
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
        f.merge(indexed.data);
        f.save();

        // Plant what a lost project table write leaves behind: a manifest carrying
        // a generation the persisted project table never reached. Every
        // FileVersion it references is known and its shard variant stored
        // (a shard-only reindex), so only the generation can tell that the
        // global's symbols never committed.
        auto tu_id = f.project.file_table.intern(Spelling::absolute(src));
        auto raced = *f.project.index.held_manifest(tu_id);
        raced.table_generation = f.project.index.table_generation() + 1;
        auto bytes = index::serialize_manifest(f.project.index.export_manifest(raced));
        // Keyed by the interned (canonical) spelling, like save() itself:
        // on Windows the raw TempDir spelling hashes to a different key.
        f.project.database->write(
            {
                {index::BlobKind::TUManifest,
                 index::blob_key(f.project.file_table.resolve(tu_id)),
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
    ZASSERT(f.project.index.manifest_count() == 0);
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
        f.merge(indexed.data);
        f.save();

        // Plant what a failed manifest write under a committed global leaves
        // behind: the previous manifest, of an older generation, with every
        // FileVersion still resolvable and its shard variant stored (a
        // reindex that changed shards or the include tree only). Only the
        // global's pin can tell it is not the manifest the save meant.
        auto tu_id = f.project.file_table.intern(Spelling::absolute(src));
        auto lost = *f.project.index.held_manifest(tu_id);
        lost.table_generation = f.project.index.table_generation() - 1;
        auto bytes = index::serialize_manifest(f.project.index.export_manifest(lost));
        f.project.database->write(
            {
                {index::BlobKind::TUManifest,
                 index::blob_key(f.project.file_table.resolve(tu_id)),
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
    ZASSERT(f.project.index.manifest_count() == 0);
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
        f.merge(indexed.data);
        f.save();

        // Plant what a crash between save phases leaves: a manifest whose
        // dependency FileVersion the persisted project table never learned,
        // while the TU's own version is known — here for the header, whose
        // standalone index no CDB pass would ever rebuild.
        auto header_id = f.project.file_table.intern(Spelling::absolute(header));
        VersionID header_version;
        for(std::uint32_t version = 0; version < f.project.file_table.versions.size();
            version += 1) {
            if(f.project.file_table.versions[version].fid == header_id) {
                header_version = VersionID{version};
            }
        }
        ZASSERT(header_version.valid());
        index::TUManifest stale;
        stale.tu_version = header_version;
        stale = f.project.index.export_manifest(stale);
        stale.nodes.push_back({.file = 9999});
        auto bytes = index::serialize_manifest(stale);
        f.project.database->write(
            {
                {index::BlobKind::TUManifest, index::blob_key(header), std::move(bytes)}
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
    ZASSERT(!f.project.index.held_manifest(header_id));
    auto save_body = [&]() -> kota::task<> {
        co_await f.async_save();
    };
    auto task = save_body();
    f.loop.schedule(task);
    f.loop.run();
    bool stale_alive = false;
    f.project.database->for_each_key(index::BlobKind::TUManifest, [&](llvm::StringRef key) {
        stale_alive |= key == index::blob_key(header);
    });
    ZASSERT(!stale_alive);

    // The TU whose manifest resolved is untouched.
    auto tu_id = f.project.file_table.intern(Spelling::absolute(src));
    ZASSERT(f.project.index.held_manifest(tu_id) != nullptr);
    ZASSERT(!f.pump.pending_reason(tu_id).has_value());
}

ZEST_CASE(FreshWriteBeatsRemoval) {
    TempDir tmp;
    tmp.touch("main.cpp", "int sweep_target() { return 1; }\n");
    auto src = tmp.path("main.cpp");

    {
        IndexerFixture f;
        open_store(tmp, f.project);
        auto indexed = index_file(tmp, src);
        ZASSERT(!indexed.data.empty());
        f.merge(indexed.data);
        f.save();
        // The indexer keys blobs by the pool-canonical path, which need
        // not equal the raw temp path byte-for-byte (Windows 8.3 names).
        auto key = index::blob_key(f.project.file_table.resolve(
            f.project.file_table.intern(Spelling::absolute(indexed.tu_path))));
        // Replace the persisted manifest with an unresolvable one: the
        // next load removes it — deferred into the first save — and the
        // TU's shard turns orphan, deferred too.
        index::TUManifest stale;
        stale.tu_version = VersionID{999999};
        stale.nodes.push_back({.file = 9999});
        auto bytes = index::serialize_manifest(stale);
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
    f.merge(indexed.data);
    f.save();

    auto key = index::blob_key(f.project.file_table.resolve(
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

ZEST_CASE(LoadServesAcrossSaves) {
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
        f.merge(indexed.data);
        f.save();
    }

    IndexerFixture f;
    open_lmdb(f.project);
    ZASSERT(f.load());
    auto path_id = f.project.file_table.intern(Spelling::absolute(src));
    ZASSERT(f.project.index.shard(path_id) != nullptr);

    // The loaded shard borrows the open-time snapshot. A save that commits
    // anything advances and retires it — the shard must come out rebound
    // onto the fresh snapshot, still serving.
    tmp.touch("other.cpp", "int other_value() { return 2; }\n");
    auto other = index_file(tmp, tmp.path("other.cpp"));
    ZASSERT(!other.data.empty());
    f.merge(other.data);
    f.save();

    auto* it = f.project.index.shard(path_id);
    ZASSERT(it);
    ZASSERT(it->loaded());
    ZASSERT(it->content_hash() == llvm::xxh3_64bits("int lmdb_value() { return 1; }\n"));
    ZASSERT(!it->bytes().empty());
}

ZEST_CASE(CorruptTableCondemnsDatabase) {
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

        llvm::SmallVector<std::uint32_t> write(llvm::ArrayRef<Blob>,
                                               llvm::ArrayRef<index::BlobKey>) override {
            return {};
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
        f.merge(indexed.data);
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

        llvm::SmallVector<std::uint32_t> write(llvm::ArrayRef<Blob> puts,
                                               llvm::ArrayRef<index::BlobKey> removes) override {
            return real->write(puts, removes);
        }

        void for_each_key(index::BlobKind kind,
                          llvm::function_ref<void(llvm::StringRef)> fn) override {
            real->for_each_key(kind, fn);
        }

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

    IndexerFixture f;
    open_store(tmp, f.project);
    bool condemned = false;
    auto wrapper = std::make_unique<CorruptShard>();
    wrapper->real = std::move(f.project.database);
    wrapper->condemned = &condemned;
    f.project.database = std::move(wrapper);

    ZASSERT(f.load());
    ZASSERT(condemned);
    ZASSERT(f.project.index.shard_count() == 0);
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
    // Beside a project table, or the next load would find no index and
    // never read the debt back.
    ZASSERT(f.project.database->contains(index::BlobKind::ProjectTable, "global"));
}

ZEST_CASE(UnreadableTablePreserved) {
    TempDir tmp;
    tmp.touch("main.cpp", "int keep() { return 1; }\n");
    auto src = tmp.path("main.cpp");

    {
        IndexerFixture f;
        open_store(tmp, f.project);
        auto indexed = index_file(tmp, src);
        ZASSERT(!indexed.data.empty());
        f.merge(indexed.data);
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
            return kind == index::BlobKind::ProjectTable ? index::ReadBlob{}
                                                         : real->read(kind, key);
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
            return 1;
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
        ZASSERT(f.project.index.manifest_count() == 0);
        ZASSERT(f.project.database == nullptr);
    }

    IndexerFixture f;
    open_store(tmp, f.project);
    f.load();
    ZASSERT(f.project.index.manifest_count() != 0);
    ZASSERT(f.project.index.shard_count() != 0);
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
        f.merge(indexed.data);
        f.save();

        // The compile command changed: content freshness cannot see it, so
        // the TU's index is dropped wholesale and staleness flips at once.
        f.drop_index(f.project.file_table.intern(Spelling::absolute(src)));
        ZASSERT(f.project.index.manifest_count() == 0);
        ZASSERT(f.needs_reindex(src));
        f.save();
    }

    // The drop survives a restart: nothing on disk resurrects the
    // old-command index as fresh.
    IndexerFixture f;
    open_store(tmp, f.project);
    f.load();
    ZASSERT(f.project.index.manifest_count() == 0);
    ZASSERT(f.project.index.shard_count() == 0);
    ZASSERT(f.needs_reindex(src));
}

};  // ZEST_SUITE(ProjectIndexLoad)

}  // namespace
}  // namespace clice::testing

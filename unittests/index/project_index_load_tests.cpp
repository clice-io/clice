module;

#include "support/prelude.h"

module clice;

import :index.database;
import :index.format;
import :index.project_index;
import :index.symbol_query;
import :project.project;
import :sched.index.pending_table;
import :support.bytes;
import :unittests.utils.forwarding_database;
import :unittests.utils.index_internals;
import :unittests.utils.indexer_fixture;
import :unittests.utils.temp_dir;
import :unittests.utils;
import :vfs.cache_store;
import :vfs.ids;
import :vfs.path;

namespace clice::testing {
namespace {

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

ZEST_CASE(OpenReadsHeaderOnly) {
    // Opening reads the header and the search index it pins: every other
    // record is read when something asks for it.
    TempDir tmp;
    tmp.touch("dep.h", "#pragma once\ninline int dep() { return 1; }\n");
    tmp.touch("main.cpp", "#include \"dep.h\"\nint use() { return dep(); }\n");
    {
        IndexerFixture f;
        open_store(tmp, f.project);
        auto indexed = index_file(tmp, tmp.path("main.cpp"));
        ZASSERT(!indexed.data.empty());
        f.merge(indexed.data);
        f.save(index::SearchRebuild::Idle);
    }

    struct ReadLog final : ForwardingDatabase {
        using ForwardingDatabase::ForwardingDatabase;

        llvm::SmallVector<index::BlobKind> reads;

        index::ReadBlob read(index::BlobKind kind, llvm::StringRef key) override {
            reads.push_back(kind);
            return real->read(kind, key);
        }

        void for_each(index::BlobKind kind,
                      llvm::function_ref<bool(llvm::StringRef, index::ReadBlob)> fn) override {
            reads.push_back(kind);
            real->for_each(kind, fn);
        }
    };

    IndexerFixture f;
    open_store(tmp, f.project);
    auto log = std::make_unique<ReadLog>(std::move(f.project.database));
    auto* probe = log.get();
    f.project.database = std::move(log);
    ZASSERT(f.project.index.open(*f.project.database) == index::LoadOutcome::Loaded);
    ZEXPECT(probe->reads == llvm::SmallVector<index::BlobKind>{index::BlobKind::Header,
                                                               index::BlobKind::SearchIndex});
    ZEXPECT(f.project.index.search_index().loaded());
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
        // The plain save persists the records; the idle one then rebuilds
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

ZEST_CASE(StaleUnitsFollowDisk) {
    // The start-up scan finds the units a change made while no server ran
    // reaches: an edited header's includer, and a unit a failed lookup of
    // which now finds a file. The unit nothing changed under stays.
    TempDir tmp;
    tmp.touch("dep.h", "#pragma once\ninline int dep() { return 1; }\n");
    tmp.touch("main.cpp", "#include \"dep.h\"\nint use() { return dep(); }\n");
    tmp.touch("other.cpp", "int other() { return 2; }\n");
    tmp.touch("late.cpp", "#include \"made.h\"\nint late() { return 3; }\n");
    tmp.mkdir("gen");
    {
        IndexerFixture f;
        open_store(tmp, f.project);
        for(auto name: {"main.cpp", "other.cpp"}) {
            auto indexed = index_file(tmp, tmp.path(name));
            ZASSERT(!indexed.data.empty());
            ZASSERT(f.merge(indexed.data));
        }
        auto late = index_file(tmp, tmp.path("late.cpp"), {"-I" + tmp.path("gen")});
        ZASSERT(!late.data.empty());
        ZASSERT(f.merge(late.data));
        f.save();
    }
    tmp.touch("dep.h", "#pragma once\ninline int dep() { return 100; }\n");
    tmp.touch("gen/made.h", "#pragma once\n");

    IndexerFixture f;
    open_store(tmp, f.project);
    f.load();
    std::vector<std::string> stale;
    for(auto unit: f.project.index.stale_units()) {
        stale.push_back(llvm::sys::path::filename(f.project.file_table.resolve(unit)).str());
    }
    ranges::sort(stale);
    ZEXPECT(stale == std::vector<std::string>{"late.cpp", "main.cpp"});
    auto file_of = [&](llvm::StringRef name) {
        return f.project.file_table.intern(Spelling::absolute(tmp.path(name)));
    };
    ZEXPECT(IndexInternals::behind(f.project.index, file_of("main.cpp")));
    ZEXPECT(!IndexInternals::behind(f.project.index, file_of("other.cpp")));
}

ZEST_CASE(OwedUnitsPersist) {
    // A rebuild of a shared header discards another unit's variant; that
    // unit reads fresh by content, so the debt survives a restart in the
    // header until the unit merges again.
    TempDir tmp;
    tmp.touch("gen.h",
              "#pragma once\n#ifdef MODE\nint gen_mode();\n#endif\n"
              "inline int gen_fn() { return 1; }\n");
    tmp.touch("ga.cpp", "#include \"gen.h\"\nint ga() { return gen_fn(); }\n");
    tmp.touch("gb.cpp", "#include \"gen.h\"\nint gb() { return gen_fn(); }\n");
    auto gb_unit = [&](IndexerFixture& f) {
        return f.project.file_table.intern(Spelling::absolute(tmp.path("gb.cpp")));
    };
    {
        IndexerFixture f;
        open_store(tmp, f.project);
        auto a = index_file(tmp, tmp.path("ga.cpp"));
        auto b = index_file(tmp, tmp.path("gb.cpp"), {"-DMODE"});
        ZASSERT(f.merge(a.data));
        ZASSERT(f.merge(b.data));
        tmp.touch("gen.h",
                  "#pragma once\n#ifdef MODE\nint gen_mode();\n#endif\n"
                  "inline int gen_fn() { return 2; }\n");
        ZASSERT(f.merge(index_file(tmp, tmp.path("ga.cpp")).data));
        ZASSERT(f.project.index.owed_units().contains(gb_unit(f)));
        f.save();
    }

    IndexerFixture f;
    open_store(tmp, f.project);
    f.load();
    ZEXPECT(f.project.index.owed_units().contains(gb_unit(f)));
    ZASSERT(f.merge(index_file(tmp, tmp.path("gb.cpp"), {"-DMODE"}).data));
    ZEXPECT(f.project.index.owed_units().empty());
}

ZEST_CASE(PersistedInternalsSearched) {
    // A later session's search build reads the names of internal linkage
    // from the records an earlier session saved.
    TempDir tmp;
    tmp.touch("b.cpp", "static int rows = 2;\nint b() { return rows; }\n");
    {
        IndexerFixture f;
        open_store(tmp, f.project);
        auto indexed = index_file(tmp, tmp.path("b.cpp"));
        ZASSERT(!indexed.data.empty());
        ZASSERT(f.merge(indexed.data));
        f.save();
        ZASSERT(!f.project.index.search_index().loaded());
    }

    IndexerFixture f;
    open_store(tmp, f.project);
    f.load();
    f.save(index::SearchRebuild::Idle);
    ZASSERT(f.project.index.search_index().loaded());
    ZEXPECT(f.project.index.search_pending_files().empty());
    auto hits = f.project.index.search_index()
                    .search(*index::SymbolQuery::parse("rows"), 10, f.project.index.workspace)
                    .hits;
    ZASSERT(hits.size() == 1u);
    ZEXPECT(hits.front().file.ends_with("b.cpp"));
}

ZEST_CASE(PathIdsAcrossSessions) {
    // Each session names the files it meets first by ids after the ones the
    // database handed out: a later session's files never take an earlier
    // one's ids, and every file reads back under its own name.
    TempDir tmp;
    tmp.touch("a.h", "#pragma once\ninline int from_a() { return 1; }\n");
    tmp.touch("a.cpp", "#include \"a.h\"\nint use_a() { return from_a(); }\n");
    tmp.touch("b.h", "#pragma once\ninline int from_b() { return 2; }\n");
    tmp.touch("b.cpp", "#include \"b.h\"\nint use_b() { return from_b(); }\n");
    for(auto name: {"a.cpp", "b.cpp"}) {
        IndexerFixture f;
        open_store(tmp, f.project);
        f.load();
        auto indexed = index_file(tmp, tmp.path(name));
        ZASSERT(!indexed.data.empty());
        ZASSERT(f.merge(indexed.data));
        f.save();
    }

    IndexerFixture f;
    open_store(tmp, f.project);
    f.load();
    auto file_of = [&](llvm::StringRef name) {
        return f.project.file_table.intern(Spelling::absolute(tmp.path(name)));
    };
    auto declared_in = [&](llvm::StringRef symbol) {
        Fid file;
        f.project.index.for_each_symbol(
            [&](index::SymbolHash, const index::SymbolView& view, std::uint32_t) {
                if(view.name == symbol) {
                    file = Fid{view.file};
                }
                return !file.valid();
            });
        return file;
    };
    ZEXPECT(declared_in("from_a") == file_of("a.h"));
    ZEXPECT(declared_in("from_b") == file_of("b.h"));
    ZEXPECT(contributions_of(f.project.index, file_of("a.h")).contains(file_of("a.cpp")));
    ZEXPECT(contributions_of(f.project.index, file_of("b.h")).contains(file_of("b.cpp")));
    ZEXPECT(!contributions_of(f.project.index, file_of("b.h")).contains(file_of("a.cpp")));
}

ZEST_CASE(DamagedRecordRebuildsAll) {
    // A record failing its checks reads as missing and damages the
    // database: the next save discards it, and every unit is indexed again
    // into a fresh one — the build's, and one of its own no build entry
    // names.
    TempDir tmp;
    tmp.touch("dep.h", "#pragma once\ninline int dep() { return 1; }\n");
    tmp.touch("main.cpp", "#include \"dep.h\"\nint use() { return dep(); }\n");
    tmp.touch("solo.cpp", "int solo() { return 2; }\n");
    auto src = tmp.path("main.cpp");
    auto command = [&](IndexerFixture& f) {
        f.project.cdb.add_command(tmp.root, src, llvm::StringRef("clang++ -c main.cpp"));
    };
    std::string header_key;
    {
        IndexerFixture f;
        open_store(tmp, f.project);
        command(f);
        auto indexed = index_file(tmp, src);
        ZASSERT(!indexed.data.empty());
        f.merge(indexed.data);
        ZASSERT(f.merge(index_file(tmp, tmp.path("solo.cpp")).data));
        f.save();
        header_key = IndexInternals::file_key(
            f.project.index,
            f.project.file_table.intern(Spelling::absolute(tmp.path("dep.h"))));
    }
    inject_blob(tmp, index::BlobKind::Shard, header_key, "corrupted beyond verification");

    IndexerFixture f;
    open_store(tmp, f.project);
    command(f);
    f.load();
    auto solo = f.project.file_table.intern(Spelling::absolute(tmp.path("solo.cpp")));
    auto tu_id = f.project.file_table.intern(Spelling::absolute(src));
    auto header_id = f.project.file_table.intern(Spelling::absolute(tmp.path("dep.h")));
    ZASSERT(f.project.index.shard(tu_id) != nullptr);
    ZEXPECT(!f.project.index.damaged());
    ZEXPECT(!f.project.index.shard(header_id));
    ZEXPECT(f.project.index.damaged());

    f.save();
    ZEXPECT(!f.project.index.damaged());
    ZEXPECT(!f.project.index.indexed(tu_id));
    ZEXPECT(f.pump.pending_reason(tu_id) == ReindexReason::ContentChanged);
    ZEXPECT(f.pump.pending_reason(solo) == ReindexReason::ContentChanged);
    ZEXPECT(!f.project.database->contains(index::BlobKind::Shard, header_key));
}

ZEST_CASE(MissingShardDamages) {
    // A file record naming a shard the database lacks is damage like a
    // shard failing its checks.
    TempDir tmp;
    tmp.touch("dep.h", "#pragma once\ninline int dep() { return 1; }\n");
    tmp.touch("main.cpp", "#include \"dep.h\"\nint use() { return dep(); }\n");
    std::string header_key;
    {
        IndexerFixture f;
        open_store(tmp, f.project);
        auto indexed = index_file(tmp, tmp.path("main.cpp"));
        ZASSERT(!indexed.data.empty());
        f.merge(indexed.data);
        f.save();
        header_key = IndexInternals::file_key(
            f.project.index,
            f.project.file_table.intern(Spelling::absolute(tmp.path("dep.h"))));
        ZASSERT(f.project.database->write(
            {
        },
            {{index::BlobKind::Shard, header_key}}));
    }

    IndexerFixture f;
    open_store(tmp, f.project);
    f.load();
    ZEXPECT(
        !f.project.index.shard(f.project.file_table.intern(Spelling::absolute(tmp.path("dep.h")))));
    ZEXPECT(f.project.index.damaged());
}

ZEST_CASE(LoadDamageStartsOver) {
    // The load's own reads (the scan of the file records) meet the damaged
    // record: the database is discarded before anything is served from it.
    TempDir tmp;
    tmp.touch("main.cpp", "int lone() { return 1; }\n");
    auto src = tmp.path("main.cpp");
    std::string key;
    {
        IndexerFixture f;
        open_store(tmp, f.project);
        auto indexed = index_file(tmp, src);
        ZASSERT(!indexed.data.empty());
        f.merge(indexed.data);
        f.save();
        key = IndexInternals::file_key(f.project.index,
                                       f.project.file_table.intern(Spelling::absolute(src)));
    }
    inject_blob(tmp, index::BlobKind::File, key, "not a file record");

    IndexerFixture f;
    open_store(tmp, f.project);
    f.load();
    ZEXPECT(!f.project.index.damaged());
    ZEXPECT(!f.project.index.indexed(f.project.file_table.intern(Spelling::absolute(src))));
    ZEXPECT(!f.project.database->contains(index::BlobKind::Header, "header"));
}

ZEST_CASE(OtherFormatDiscardsAll) {
    // A database another format version wrote is discarded whole, the
    // context choices and artifact records with it.
    TempDir tmp;
    index::HeaderBlob header;
    header.format_version = index::index_format_version + 1;
    auto bytes = index::encode(header);
    inject_blob(tmp, index::BlobKind::Header, "header", as_chars(bytes));
    inject_blob(tmp, index::BlobKind::ContextChoices, "contexts", "choices");

    IndexerFixture f;
    open_store(tmp, f.project);
    ZEXPECT(!f.load());
    ZEXPECT(f.project.contexts.bytes.empty());
    ZEXPECT(!f.project.database->contains(index::BlobKind::ContextChoices, "contexts"));
    ZEXPECT(!f.project.database->contains(index::BlobKind::Header, "header"));
}

ZEST_CASE(ReadOnlyLoadKeepsDisk) {
    // A reader beside a writer finds a damaged record: it reports the damage
    // and leaves the database to its owner.
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
        header_key = IndexInternals::file_key(
            f.project.index,
            f.project.file_table.intern(Spelling::absolute(tmp.path("dep.h"))));
    }
    inject_blob(tmp, index::BlobKind::Shard, header_key, "corrupted beyond verification");

    IndexerFixture f;
    auto store = CacheStore::open(tmp.path("cache"), 1);
    ZASSERT(store);
    f.project.store.emplace(std::move(*store));
    f.project.database = index::open_database(*f.project.store, "", /*read_only=*/true);
    ZASSERT(f.project.database != nullptr);
    f.load();

    auto header_id = f.project.file_table.intern(Spelling::absolute(tmp.path("dep.h")));
    ZEXPECT(!f.project.index.shard(header_id));
    ZEXPECT(f.project.index.damaged());
    f.save();
    ZEXPECT(f.project.database->contains(index::BlobKind::Shard, header_key));
    ZEXPECT(f.project.database->contains(index::BlobKind::Header, "header"));
}

ZEST_CASE(LoadServesAcrossSaves) {
    TempDir tmp;
    tmp.touch("main.cpp", "int lmdb_value() { return 1; }\n");
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

ZEST_CASE(CorruptHeaderCondemnsDatabase) {
    // Page corruption under the header: the read fails, contains() still
    // says present, corrupted() confirms. The load condemns the database
    // (deleted on close) and continues on a fresh empty one instead of
    // running without persistence forever.
    struct CorruptHeader final : ForwardingDatabase {
        using ForwardingDatabase::ForwardingDatabase;

        bool* condemned;

        index::ReadBlob read(index::BlobKind kind, llvm::StringRef key) override {
            return kind == index::BlobKind::Header ? index::ReadBlob{} : real->read(kind, key);
        }

        bool contains(index::BlobKind kind, llvm::StringRef key) override {
            return kind == index::BlobKind::Header || real->contains(kind, key);
        }

        bool corrupted() const override {
            return true;
        }

        void condemn() override {
            *condemned = true;
            real->condemn();
        }
    };

    TempDir tmp;
    IndexerFixture f;
    open_store(tmp, f.project);
    bool condemned = false;
    auto spy = std::make_unique<CorruptHeader>(std::move(f.project.database));
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
    auto command = [&](IndexerFixture& f) {
        f.project.cdb.add_command(tmp.root, src, llvm::StringRef("clang++ -c main.cpp"));
    };

    {
        IndexerFixture f;
        open_store(tmp, f.project);
        command(f);
        auto indexed = index_file(tmp, src);
        ZASSERT(!indexed.data.empty());
        f.merge(indexed.data);
        f.save();
    }

    // Page corruption a shard read meets, long after the load: the next
    // save condemns the database and the project starts over.
    struct CorruptShard final : ForwardingDatabase {
        using ForwardingDatabase::ForwardingDatabase;

        bool* condemned;
        bool poisoned = false;

        index::ReadBlob read(index::BlobKind kind, llvm::StringRef key) override {
            if(kind == index::BlobKind::Shard) {
                poisoned = true;
                return {};
            }
            return real->read(kind, key);
        }

        bool corrupted() const override {
            return poisoned;
        }

        void condemn() override {
            *condemned = true;
            real->condemn();
        }
    };

    IndexerFixture f;
    open_store(tmp, f.project);
    command(f);
    bool condemned = false;
    auto wrapper = std::make_unique<CorruptShard>(std::move(f.project.database));
    wrapper->condemned = &condemned;
    f.project.database = std::move(wrapper);

    ZASSERT(f.load());
    auto tu_id = f.project.file_table.intern(Spelling::absolute(src));
    ZASSERT(!f.project.index.shard(tu_id));
    f.save();
    ZASSERT(condemned);
    ZASSERT(f.project.database != nullptr);
    ZASSERT(!f.project.index.indexed(tu_id));
    ZASSERT(f.pump.pending_reason(tu_id) == ReindexReason::ContentChanged);
}

ZEST_CASE(UnreadableHeaderPreserved) {
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

    // The header exists but fails to read — a transient IO error at
    // startup, not absence. Removing everything would destroy the intact
    // index; a fresh lineage saved over the unread one could alias its path
    // ids. The session runs memory-only and leaves every record for the
    // next start.
    struct UnreadableHeader final : ForwardingDatabase {
        using ForwardingDatabase::ForwardingDatabase;

        index::ReadBlob read(index::BlobKind kind, llvm::StringRef key) override {
            return kind == index::BlobKind::Header ? index::ReadBlob{} : real->read(kind, key);
        }
    };

    auto tu_id = [](IndexerFixture& f, llvm::StringRef path) {
        return f.project.file_table.intern(Spelling::absolute(path));
    };
    {
        IndexerFixture f;
        open_store(tmp, f.project);
        f.project.database = std::make_unique<UnreadableHeader>(std::move(f.project.database));
        f.load();
        ZASSERT(!f.project.index.indexed(tu_id(f, src)));
        ZASSERT(f.project.database == nullptr);
    }

    IndexerFixture f;
    open_store(tmp, f.project);
    f.load();
    ZASSERT(f.project.index.indexed(tu_id(f, src)));
    ZASSERT(f.project.index.shard(tu_id(f, src)) != nullptr);
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
        ZASSERT(!f.project.index.indexed(f.project.file_table.intern(Spelling::absolute(src))));
        ZASSERT(f.needs_reindex(src));
        f.save();
    }

    // The drop survives a restart: nothing on disk resurrects the
    // old-command index as fresh.
    IndexerFixture f;
    open_store(tmp, f.project);
    f.load();
    ZASSERT(!f.project.index.indexed(f.project.file_table.intern(Spelling::absolute(src))));
    ZASSERT(f.project.index.shard_count() == 0);
    ZASSERT(f.needs_reindex(src));
}

};  // ZEST_SUITE(ProjectIndexLoad)

}  // namespace
}  // namespace clice::testing

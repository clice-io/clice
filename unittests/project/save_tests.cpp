module clice;

import :index.database;
import :index.project_index;
import :index.symbol_query;
import :project.contexts;
import :project.project;
import :project.save;
import :sched.index.pending_table;
import :unittests.utils.forwarding_database;
import :unittests.utils.indexer_fixture;
import :unittests.utils.temp_dir;
import :unittests.utils;
import :vfs.path;

namespace clice::testing {
namespace {

ZEST_SUITE(ContextChoices) {

ZEST_CASE(ContextsBlobRoundTrip) {
    // The editor's choices ride the context choices: a restart attaching an
    // editor gets them back, and the save that commits them leaves nothing
    // pending.
    TempDir tmp;
    tmp.touch("host.cpp", "#include \"h.h\"\n");
    tmp.touch("h.h", "int x;\n");
    auto host_path = tmp.path("host.cpp");
    auto header_path = tmp.path("h.h");

    {
        IndexerFixture f;
        open_store(tmp, f.project);
        f.load();
        auto host = f.project.file_table.intern(Spelling::absolute(host_path));
        auto header = f.project.file_table.intern(Spelling::absolute(header_path));
        ContextChoice choice{host, 1, "applied", "base"};
        std::pair<Fid, const ContextChoice*> choices[] = {
            {header, &choice}
        };
        f.project.choices.bytes = serialize_choices(f.project, choices);
        f.project.choices.dirty = true;
        f.save();
        ZASSERT(!f.project.choices.dirty);
    }

    IndexerFixture f;
    open_store(tmp, f.project);
    f.load();
    auto host = f.project.file_table.intern(Spelling::absolute(host_path));
    auto header = f.project.file_table.intern(Spelling::absolute(header_path));
    auto choices = parse_choices(f.project, f.project.choices.bytes);
    ZASSERT(choices.size() == 1u);
    ZASSERT(choices[0].first == header);
    auto& saved = choices[0].second;
    ZASSERT(saved.host_path_id == host);
    ZASSERT(saved.occurrence == std::optional<std::uint32_t>(1));
    ZASSERT(saved.command_hash == "applied");
    ZASSERT(saved.base_hash == "base");
    ZASSERT(!f.project.choices.dirty);
}

ZEST_CASE(CorruptSaveKeepsPending) {
    // A write that commits the context choices into a database it then
    // finds corrupt made nothing durable: the change stays pending until the
    // database opened in its place takes them.
    struct CorruptAfterWrite final : ForwardingDatabase {
        using ForwardingDatabase::ForwardingDatabase;

        bool written = false;

        bool write(llvm::ArrayRef<Blob> puts, llvm::ArrayRef<index::BlobKey> removes) override {
            written = real->write(puts, removes);
            return written;
        }

        bool corrupted() const override {
            return written;
        }
    };

    TempDir tmp;
    IndexerFixture f;
    open_store(tmp, f.project);
    f.load();
    f.project.database = std::make_unique<CorruptAfterWrite>(std::move(f.project.database));
    f.project.choices.bytes = "choices";
    f.project.choices.dirty = true;

    f.save();
    ZASSERT(f.project.choices.dirty);

    f.save();
    ZASSERT(!f.project.choices.dirty);
    auto kept = f.project.database->read(index::BlobKind::ContextChoices, "contexts");
    ZASSERT(bool(kept));
    ZASSERT(kept.buffer->getBuffer() == "choices");
}

ZEST_CASE(UnownedContextsPassThrough) {
    // A process with no editor (the batch commands) never rewrites the
    // context choices, yet carries their bytes into a database replacing a
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
    // context choices, and no save keeps retrying them.
    TempDir tmp;
    IndexerFixture f;
    open_store(tmp, f.project);
    f.load();
    f.reopen_database();
    f.save();
    ZASSERT(!bool(f.project.database->read(index::BlobKind::ContextChoices, "contexts")));
}

};  // ZEST_SUITE(ContextChoices)

/// A database reporting its pages as `pages`, `free` of them free, and
/// recording the compact copies written and what their swaps did;
/// `forced_copy` and `forced_swap` stand in for their results. A lost
/// handle reads nothing, nor does a replaced file under `lose_reads`, as
/// views diverging from it would.
struct FreeSpace final : ForwardingDatabase {
    using ForwardingDatabase::ForwardingDatabase;

    std::uint64_t pages = 1 << 15;

    std::uint64_t free = 1 << 14;

    std::uint32_t copies = 0;

    std::optional<Swap> swapped;

    std::optional<Copy> forced_copy;

    std::optional<Swap> forced_swap;

    bool lose_reads = false;

    std::optional<index::PageUsage> usage() override {
        auto usage = real->usage();
        if(usage) {
            usage->pages = pages;
            usage->free_pages = free;
        }
        return usage;
    }

    Copy write_compact_copy() override {
        copies += 1;
        return forced_copy ? *forced_copy : real->write_compact_copy();
    }

    Swap swap_compact_copy() override {
        swapped = forced_swap ? *forced_swap : real->swap_compact_copy();
        return *swapped;
    }

    index::ReadBlob read(index::BlobKind kind, llvm::StringRef key) override {
        if(lost()) {
            return {};
        }
        return real->read(kind, key);
    }

    void for_each(index::BlobKind kind,
                  llvm::function_ref<bool(llvm::StringRef, index::ReadBlob)> fn) override {
        if(!lost()) {
            real->for_each(kind, fn);
        }
    }

    bool lost() const {
        return swapped == Swap::Lost || (lose_reads && swapped == Swap::Replaced);
    }
};

/// A project of a header large enough for its shard to be read in place
/// from the database, a unit including it, and a second unit.
struct ShrinkProject {
    TempDir tmp;
    IndexerFixture f;

    ShrinkProject() {
        std::string header = "#pragma once\n";
        for(int i = 0; i < 2000; i += 1) {
            header += std::format("inline int function_{0}(int x) {{ return x + {0}; }}\n", i);
        }
        tmp.touch("big.h", header);
        tmp.touch("a.cpp",
                  "#include \"big.h\"\nstatic int first_local = 1;\n"
                  "int first_value() { return function_1(first_local); }\n");
        tmp.touch("b.cpp", "int second_value() { return 2; }\n");
        open_store(tmp, f.project);
        ZASSERT(f.merge(index_file(tmp, tmp.path("a.cpp")).data));
        f.save(index::SearchRebuild::Idle);
    }

    FreeSpace& spy() {
        auto spy = std::make_unique<FreeSpace>(std::move(f.project.database));
        auto& space = *spy;
        f.project.database = std::move(spy);
        return space;
    }

    index::IndexReport shrink() {
        index::IndexReport report;
        auto run = [&]() -> async::task<> {
            report = co_await shrink_database(f.project, f.loop);
        };
        auto task = run();
        f.loop.schedule(task);
        f.loop.run();
        return report;
    }

    std::size_t hits(llvm::StringRef name) {
        return f.project.index.search_index()
            .search(*index::SymbolQuery::parse(name), 10, {})
            .hits.size();
    }

    Fid unit(llvm::StringRef name) {
        return f.project.file_table.intern(Spelling::absolute(tmp.path(name)));
    }
};

ZEST_SUITE(ShrinkDatabase) {

ZEST_CASE(ShrinkThresholds) {
    // A quarter of the file free, and 64 MB at least.
    auto due = [](std::uint64_t pages, std::uint64_t free) {
        return shrink_due({.page_size = 4096, .pages = pages, .free_pages = free});
    };
    ZEXPECT(due(1 << 16, 1 << 14));
    ZEXPECT(!due(1 << 16, (1 << 14) - 1));
    ZEXPECT(!due(1 << 15, (1 << 14) - 1));
    ZEXPECT(due(1 << 20, 1 << 18));
    ZEXPECT(!due(1 << 20, (1 << 18) - 1));
}

ZEST_CASE(ShrinkKeepsIndex) {
    // The index reads on from the compact copy, its shards and search
    // segments in place, and saves commit into it.
    ShrinkProject p;
    auto& space = p.spy();
    auto report = p.shrink();
    ZEXPECT(space.copies == 1u);
    ZEXPECT(space.swapped == index::BlobDatabase::Swap::Replaced);
    ZEXPECT(report.reindex().empty());
    ZEXPECT(!p.f.project.index.damaged());
    ZEXPECT(p.hits("first_local") == 1u);
    auto* shard = p.f.project.index.shard(p.unit("big.h"));
    ZASSERT(shard != nullptr);
    ZEXPECT(shard->content_hash() != 0u);

    ZASSERT(p.f.merge(index_file(p.tmp, p.tmp.path("b.cpp")).data));
    p.f.save(index::SearchRebuild::Idle);
    ZEXPECT(p.hits("second_value") == 1u);
    ZEXPECT(!p.f.project.index.has_unsaved_state());
}

ZEST_CASE(LittleFreeKeepsFile) {
    ShrinkProject p;
    auto& space = p.spy();
    space.free = 1 << 10;
    p.shrink();
    ZEXPECT(space.copies == 0u);
}

ZEST_CASE(LostDatabaseStartsOver) {
    // A database that does not open again after the swap, or a view that no
    // longer matches it, is discarded: every unit is indexed again.
    for(bool reopened: {false, true}) {
        ShrinkProject p;
        auto& space = p.spy();
        if(reopened) {
            space.lose_reads = true;
        } else {
            space.forced_swap = index::BlobDatabase::Swap::Lost;
        }
        auto report = p.shrink();
        ZEXPECT(ranges::contains(report.reindex(), p.unit("a.cpp")));
        ZEXPECT(!p.f.project.index.search_index().loaded());
        ZEXPECT(p.f.project.database != nullptr);
    }
}

ZEST_CASE(FailedShrinkStops) {
    // A copy that failed, or could not replace the file, is not tried again
    // in the session; another process holding the database open only
    // defers it.
    using Database = index::BlobDatabase;
    for(auto failure: {Database::Copy::Failed, Database::Copy::Busy}) {
        ShrinkProject p;
        auto& space = p.spy();
        space.forced_copy = failure;
        p.shrink();
        space.forced_copy.reset();
        p.shrink();
        bool failed = failure == Database::Copy::Failed;
        ZEXPECT(space.copies == (failed ? 1u : 2u));
        ZEXPECT(p.f.project.shrink_failed == failed);
    }
    ShrinkProject p;
    auto& space = p.spy();
    space.forced_swap = Database::Swap::Kept;
    p.shrink();
    p.shrink();
    ZEXPECT(space.copies == 1u);
    ZEXPECT(p.hits("first_local") == 1u);
}

ZEST_CASE(SaveWaitsForShrink) {
    // A save arriving while the copy is written waits for the swap, and
    // commits into the compact copy.
    ShrinkProject p;
    auto& space = p.spy();
    ZASSERT(p.f.merge(index_file(p.tmp, p.tmp.path("b.cpp")).data));
    auto shrink = [&]() -> async::task<> {
        co_await shrink_database(p.f.project, p.f.loop);
    };
    auto shrink_task = shrink();
    auto save_task = p.f.async_save(index::SearchRebuild::Idle);
    p.f.loop.schedule(shrink_task);
    p.f.loop.schedule(save_task);
    p.f.loop.run();
    ZEXPECT(space.swapped == index::BlobDatabase::Swap::Replaced);
    ZEXPECT(!p.f.project.index.has_unsaved_state());
    // One process opens a database once at a time.
    p.f.project.database.reset();
    IndexerFixture again;
    open_store(p.tmp, again.project);
    again.load();
    ZEXPECT(again.project.index.search_index()
                .search(*index::SymbolQuery::parse("second_value"), 10, {})
                .hits.size() == 1u);
}

ZEST_CASE(IdleSaveShrinks) {
    // Only a save after which no indexing is queued shrinks the file.
    ShrinkProject p;
    auto& space = p.spy();
    auto persist = [&](index::SearchRebuild search) {
        auto task = p.f.pump.persist(search);
        p.f.loop.schedule(task);
        p.f.loop.run();
    };
    persist(index::SearchRebuild::Never);
    persist(index::SearchRebuild::Behind);
    ZEXPECT(space.copies == 0u);
    persist(index::SearchRebuild::Idle);
    ZEXPECT(space.copies == 1u);
    p.f.pump.enqueue(p.unit("b.cpp"), ReindexReason::ContentChanged);
    persist(index::SearchRebuild::Idle);
    ZEXPECT(space.copies == 1u);
}

ZEST_CASE(LostShrinkSavesAgain) {
    // The database opened in place of a lost one takes the context
    // choices at once: no indexing need follow to save them.
    ShrinkProject p;
    auto& space = p.spy();
    space.forced_swap = index::BlobDatabase::Swap::Lost;
    p.f.project.choices.bytes = "choices";
    p.f.project.choices.dirty = true;
    auto task = p.f.pump.persist(index::SearchRebuild::Idle);
    p.f.loop.schedule(task);
    p.f.loop.run();
    ZEXPECT(!p.f.project.choices.dirty);
    auto kept = p.f.project.database->read(index::BlobKind::ContextChoices, "contexts");
    ZASSERT(bool(kept));
    ZEXPECT(kept.buffer->getBuffer() == "choices");
}

};  // ZEST_SUITE(ShrinkDatabase)

}  // namespace
}  // namespace clice::testing

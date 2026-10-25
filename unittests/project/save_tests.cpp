module;

#include "support/prelude.h"

module clice;

import :index.database;
import :index.project_index;
import :index.symbol_query;
import :project.project;
import :project.save;
import :server.editor_context;
import :unittests.utils.forwarding_database;
import :unittests.utils.indexer_fixture;
import :unittests.utils.temp_dir;
import :unittests.utils;
import :vfs.path;

namespace clice::testing {
namespace {

ZEST_SUITE(ContextChoices) {

ZEST_CASE(ContextsBlobRoundTrip) {
    // The editor's choices and artifact hosts ride the context choices: a
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
        EditorContext editor{f.project, f.commands, f.project.contexts};
        f.load();
        editor.load();
        auto host = f.project.file_table.intern(Spelling::absolute(host_path));
        auto header = f.project.file_table.intern(Spelling::absolute(header_path));
        editor.selections[header] = Selection{host, 1, "applied", "base"};
        editor.mark_dirty();
        auto ticket = f.project.contexts.ticket;
        f.save();
        ZASSERT(!f.project.contexts.dirty);
        ZASSERT(f.project.contexts.committed_ticket == ticket);
    }

    IndexerFixture f;
    open_store(tmp, f.project);
    EditorContext editor{f.project, f.commands, f.project.contexts};
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
    ZASSERT(!f.project.contexts.dirty);
}

ZEST_CASE(CorruptSaveKeepsTicket) {
    // A write that commits the context choices into a database it then
    // finds corrupt made nothing durable: the ticket stays open until the
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
    f.project.contexts.bytes = "choices";
    f.project.contexts.dirty = true;
    f.project.contexts.ticket += 1;
    auto ticket = f.project.contexts.ticket;

    f.save();
    ZASSERT(f.project.contexts.committed_ticket < ticket);
    ZASSERT(f.project.contexts.dirty);

    f.save();
    ZASSERT(f.project.contexts.committed_ticket == ticket);
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
/// counting the compact copies written.
struct FreeSpace final : ForwardingDatabase {
    using ForwardingDatabase::ForwardingDatabase;

    std::uint64_t pages = 0;

    std::uint64_t free = 0;

    std::uint32_t copies = 0;

    std::optional<index::PageUsage> usage() override {
        auto usage = real->usage();
        if(usage) {
            usage->pages = pages;
            usage->free_pages = free;
        }
        return usage;
    }

    bool write_compact_copy() override {
        copies += 1;
        return real->write_compact_copy();
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
    // The index reads on from the compact copy, and saves commit into it.
    TempDir tmp;
    tmp.touch("a.cpp", "int first_value() { return 1; }\nstatic int first_local = 1;\n");
    tmp.touch("b.cpp", "int second_value() { return 2; }\n");
    IndexerFixture f;
    open_store(tmp, f.project);
    ZASSERT(f.merge(index_file(tmp, tmp.path("a.cpp")).data));
    f.save(index::SearchRebuild::Idle);
    auto spy = std::make_unique<FreeSpace>(std::move(f.project.database));
    auto& space = *spy;
    space.pages = 1 << 15;
    space.free = 1 << 14;
    f.project.database = std::move(spy);

    auto shrink = [&]() -> kota::task<> {
        auto report = co_await shrink_database(f.project, f.loop);
        ZEXPECT(report.reindex().empty());
    };
    auto task = shrink();
    f.loop.schedule(task);
    f.loop.run();
    ZEXPECT(space.copies == 1u);
    ZEXPECT(!f.project.index.damaged());
    auto hits = [&](llvm::StringRef name) {
        return f.project.index.search_index()
            .search(*index::SymbolQuery::parse(name), 10, {})
            .hits.size();
    };
    ZEXPECT(hits("first_local") == 1u);
    auto a = f.project.file_table.intern(Spelling::absolute(tmp.path("a.cpp")));
    auto* shard = f.project.index.shard(a);
    ZASSERT(shard != nullptr);
    ZEXPECT(shard->content_hash() != 0u);

    ZASSERT(f.merge(index_file(tmp, tmp.path("b.cpp")).data));
    f.save(index::SearchRebuild::Idle);
    ZEXPECT(hits("second_value") == 1u);
    ZEXPECT(!f.project.index.has_unsaved_state());
}

ZEST_CASE(LittleFreeKeepsFile) {
    TempDir tmp;
    tmp.touch("a.cpp", "int first_value() { return 1; }\n");
    IndexerFixture f;
    open_store(tmp, f.project);
    ZASSERT(f.merge(index_file(tmp, tmp.path("a.cpp")).data));
    f.save(index::SearchRebuild::Idle);
    auto spy = std::make_unique<FreeSpace>(std::move(f.project.database));
    auto& space = *spy;
    space.pages = 1 << 15;
    space.free = 1 << 10;
    f.project.database = std::move(spy);

    auto shrink = [&]() -> kota::task<> {
        co_await shrink_database(f.project, f.loop);
    };
    auto task = shrink();
    f.loop.schedule(task);
    f.loop.run();
    ZEXPECT(space.copies == 0u);
}

};  // ZEST_SUITE(ShrinkDatabase)

}  // namespace
}  // namespace clice::testing

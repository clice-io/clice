module clice;

import :config.config;
import :index.database;
import :index.project_index;
import :project.command_snapshot;
import :project.save;
import :sched.index.pending_table;
import :unittests.utils.cdb_helper;
import :unittests.utils.forwarding_database;
import :unittests.utils.indexer_fixture;
import :unittests.utils.temp_dir;
import :unittests.utils;
import :vfs.path;

namespace clice::testing {
namespace {

ZEST_SUITE(CommandSnapshot) {

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
        f.merge(indexed.data);
        f.save();
    }

    // The command changed while no server ran: content freshness cannot
    // see it, so the persisted CDB snapshot must catch it at load.
    IndexerFixture f;
    open_store(tmp, f.project);
    f.project.cdb.add_command(tmp.root, src, llvm::StringRef("clang++ -DFOO=2 -c main.cpp"));
    f.load();

    auto tu_id = f.project.file_table.intern(Spelling::absolute(src));
    ZASSERT(!f.project.index.indexed(tu_id));
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
        f.merge(indexed.data);
        f.save();
    }

    IndexerFixture f;
    open_store(tmp, f.project);
    f.project.cdb.add_command(tmp.root, src, llvm::StringRef("clang++ -DFOO=1 -c main.cpp"));
    f.load();

    auto tu_id = f.project.file_table.intern(Spelling::absolute(src));
    ZASSERT(f.project.index.indexed(tu_id));
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
        f.merge(indexed.data);
        f.save();
    }

    // The entry vanished from the CDB: the last-known index still serves
    // navigation, same conservative semantics as the live reload path.
    IndexerFixture f;
    open_store(tmp, f.project);
    f.load();

    auto tu_id = f.project.file_table.intern(Spelling::absolute(src));
    ZASSERT(f.project.index.indexed(tu_id));
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
        ZASSERT(f.merge(indexed.data));
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
    ZASSERT(!f.project.index.indexed(tu_id));
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
        ZASSERT(f.merge(indexed.data));
        f.save();
    }

    // The file moved to the other database with the same command: its
    // index stays, and the snapshot records the database that now
    // guarantees it.
    tmp.touch("a/compile_commands.json", "[]");
    tmp.touch("b/compile_commands.json", listing);
    IndexerFixture f;
    open_store(tmp, f.project);
    load_declared(f);
    f.load();

    auto tu_id = f.project.file_table.intern(Spelling::absolute(src));
    ZASSERT(f.project.index.indexed(tu_id));
    ZASSERT(!f.pump.pending_reason(tu_id).has_value());
    ZASSERT(f.project.command_snapshot.owes_save(f.project));
    f.save();
    ZASSERT(f.project.database->advance_read_snapshot());
    auto remembered = CommandSnapshot::remembered(f.project);
    ZASSERT(remembered.sources.size() == 1);
    ZEXPECT(llvm::StringRef(remembered.sources.front().str()).ends_with("b/compile_commands.json"));
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
        ZASSERT(f.merge(indexed.data));
        f.save();
    }

    // Discovery found the database elsewhere this session and loaded it:
    // shards only the vanished one guaranteed retire, like a dropped
    // declaration's.
    fs::remove(tmp.path("compile_commands.json"));
    tmp.touch("build/compile_commands.json", listing("other.cpp"));
    IndexerFixture f;
    open_store(tmp, f.project);
    f.project.config.finalize(CanonicalPath(Spelling::absolute(tmp.root)));
    f.project.cdb.load(tmp.path("build/compile_commands.json"));
    f.load();

    auto tu_id = f.project.file_table.intern(Spelling::absolute(src));
    ZASSERT(!f.project.index.indexed(tu_id));
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
        ZASSERT(f.merge(indexed.data));
        f.save();
    }

    // The same default command covers the file again: its recorded
    // identity matches and nothing is owed.
    IndexerFixture f;
    open_store(tmp, f.project);
    add_default_rule(f);
    f.load();

    auto tu_id = f.project.file_table.intern(Spelling::absolute(src));
    ZASSERT(f.project.index.indexed(tu_id));
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
        ZASSERT(f.merge(indexed.data));
        f.save();
    }

    // The rule whose default command covered the file is gone and nothing
    // else compiles it: the index leaves instead of being rebuilt under the
    // builtin fallback, and no debt survives for a unit the build left.
    IndexerFixture f;
    open_store(tmp, f.project);
    f.load();

    auto tu_id = f.project.file_table.intern(Spelling::absolute(src));
    ZASSERT(!f.project.index.indexed(tu_id));
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
        ZASSERT(f.merge(indexed.data));
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
    ZASSERT(!f.project.index.indexed(tu_id));
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
        ZASSERT(f.merge(indexed.data));
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
    ZASSERT(!f.project.index.indexed(tu_id));
    ZASSERT(f.pump.pending_reason(tu_id) == ReindexReason::ContentChanged);
}

ZEST_CASE(HeaderIndexedAloneLeaves) {
    TempDir tmp;
    tmp.touch("dep.h", "#pragma once\ninline int dep() { return 1; }\n");
    tmp.touch("main.cpp", "#include \"dep.h\"\nint use() { return dep(); }\n");
    auto src = tmp.path("main.cpp");
    auto header = tmp.path("dep.h");

    {
        IndexerFixture f;
        open_store(tmp, f.project);
        f.project.cdb.add_command(tmp.root, src, llvm::StringRef("clang++ -c main.cpp"));
        // A pass over the header alone, as an older clice ran one for a
        // header opened before its host was indexed.
        auto indexed = index_file(tmp, header);
        ZASSERT(!indexed.data.empty());
        ZASSERT(f.merge(indexed.data));
        f.save();
    }

    // Only units are indexed: the header's own index leaves, and nothing
    // queues it, its host's run indexes it.
    IndexerFixture f;
    open_store(tmp, f.project);
    f.project.cdb.add_command(tmp.root, src, llvm::StringRef("clang++ -c main.cpp"));
    f.load();

    auto header_id = f.project.file_table.intern(Spelling::absolute(header));
    ZASSERT(!f.project.index.indexed(header_id));
    ZASSERT(!f.pump.pending_reason(header_id).has_value());
}

ZEST_CASE(UnitRuleChangeReindexed) {
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
        ZASSERT(f.merge(indexed.data));
        f.save();
    }

    // A clice.toml rule matching the unit changed offline: the unit has no
    // CDB entry, so only its own snapshot entry can see it.
    IndexerFixture f;
    open_store(tmp, f.project);
    f.project.config.rules.push_back(ConfigRule{.default_command = std::string("clang++ -c")});
    f.project.config.rules.push_back(ConfigRule{
        .patterns = {"**/*.cpp"},
        .append = {"-DFOO=1"},
    });
    f.project.config.finalize(CanonicalPath(Spelling::absolute(tmp.root)));
    f.load();

    auto tu_id = f.project.file_table.intern(Spelling::absolute(src));
    ZASSERT(!f.project.index.indexed(tu_id));
    ZASSERT(f.pump.pending_reason(tu_id) == ReindexReason::ContentChanged);
}

ZEST_CASE(CDBWriteFailureRetried) {
    TempDir tmp;
    tmp.touch("main.cpp", "int value() { return 1; }\n");
    auto src = tmp.path("main.cpp");

    // A storage refusing every batch until told otherwise.
    struct FailingStorage final : ForwardingDatabase {
        using ForwardingDatabase::ForwardingDatabase;

        bool fail = true;

        bool write(llvm::ArrayRef<Blob> puts, llvm::ArrayRef<index::BlobKey> removes) override {
            return !fail && real->write(puts, removes);
        }
    };

    IndexerFixture f;
    open_store(tmp, f.project);
    f.project.cdb.add_command(tmp.root, src, llvm::StringRef("clang++ -c main.cpp"));
    auto failing = std::make_unique<FailingStorage>(std::move(f.project.database));
    auto* storage = failing.get();
    f.project.database = std::move(failing);

    auto indexed = index_file(tmp, src);
    ZASSERT(!indexed.data.empty());
    ZASSERT(f.merge(indexed.data));
    f.save();

    // The snapshot rides the batch with the index state it describes: the
    // failed batch takes both, and both stay owed until a save commits them
    // (`clice index` fails on unsaved state after the final save).
    ZASSERT(!storage->real->contains(index::BlobKind::CommandSnapshot, "cdb"));
    ZASSERT(has_unsaved_state(f.project));
    storage->fail = false;
    f.save();
    ZASSERT(storage->real->contains(index::BlobKind::CommandSnapshot, "cdb"));
    ZASSERT(!has_unsaved_state(f.project));
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
        ZASSERT(f.merge(indexed.data));
        f.save();
        // The index committed but the final CDB write never did: the rest of
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
    ZASSERT(has_unsaved_state(f.project));
    f.save();
    ZASSERT(f.project.database->contains(index::BlobKind::CommandSnapshot, "cdb"));
    ZASSERT(!has_unsaved_state(f.project));
}

ZEST_CASE(DroppedUnitDebtRetried) {
    TempDir tmp;
    tmp.touch("main.cpp", "int value() { return 1; }\n");
    auto src = tmp.path("main.cpp");
    auto add_default_rule = [&](IndexerFixture& f, std::string command) {
        f.project.config.rules.push_back(ConfigRule{.default_command = std::move(command)});
        f.project.config.finalize(CanonicalPath(Spelling::absolute(tmp.root)));
    };

    {
        IndexerFixture f;
        open_store(tmp, f.project);
        add_default_rule(f, "clang++ -DFOO=1 -c");
        auto indexed = index_file(tmp, src);
        ZASSERT(!indexed.data.empty());
        ZASSERT(f.merge(indexed.data));
        f.save();
    }

    {
        // The default command changed offline: the unit's index is dropped
        // and queued — but the rebuild never finishes this session. The
        // saved snapshot must keep recording the unit, or nothing would
        // ever retry it.
        IndexerFixture f;
        open_store(tmp, f.project);
        add_default_rule(f, "clang++ -DFOO=2 -c");
        f.load();
        auto tu_id = f.project.file_table.intern(Spelling::absolute(src));
        ZASSERT(!f.project.index.indexed(tu_id));
        ZASSERT(f.pump.pending_reason(tu_id) == ReindexReason::ContentChanged);
        f.save();
    }

    IndexerFixture f;
    open_store(tmp, f.project);
    add_default_rule(f, "clang++ -DFOO=2 -c");
    f.load();

    auto tu_id = f.project.file_table.intern(Spelling::absolute(src));
    ZASSERT(!f.project.index.indexed(tu_id));
    ZASSERT(f.pump.pending_reason(tu_id) == ReindexReason::ContentChanged);
}

ZEST_CASE(VanishedUnitDebtDies) {
    TempDir tmp;
    tmp.touch("main.cpp", "int value() { return 1; }\n");
    auto src = tmp.path("main.cpp");
    auto add_default_rule = [&](IndexerFixture& f, std::string command) {
        f.project.config.rules.push_back(ConfigRule{.default_command = std::move(command)});
        f.project.config.finalize(CanonicalPath(Spelling::absolute(tmp.root)));
    };

    {
        IndexerFixture f;
        open_store(tmp, f.project);
        add_default_rule(f, "clang++ -DFOO=1 -c");
        auto indexed = index_file(tmp, src);
        ZASSERT(!indexed.data.empty());
        ZASSERT(f.merge(indexed.data));
        f.save();
    }

    {
        IndexerFixture f;
        open_store(tmp, f.project);
        add_default_rule(f, "clang++ -DFOO=2 -c");
        f.load();
        f.save();
    }

    // The unit was deleted while its debt entry sat in the snapshot: a
    // retry could never succeed, so the debt dies instead of keeping every
    // later run partial forever.
    ZASSERT(fs::remove(src));
    IndexerFixture f;
    open_store(tmp, f.project);
    add_default_rule(f, "clang++ -DFOO=2 -c");
    f.load();
    ZASSERT(
        !f.pump.pending_reason(f.project.file_table.intern(Spelling::absolute(src))).has_value());
}

ZEST_CASE(FailedUnitInSnapshot) {
    // A unit without entries whose index attempt failed terminally is
    // recorded nowhere but the pump's failed set; the save's debt snapshot
    // must persist it, or the repair debt dies with the process and
    // nothing ever retries the unit.
    TempDir tmp;
    tmp.touch("main.cpp", "int use() { return 0; }\n");
    tmp.touch("other.cpp", "int other() { return 0; }\n");
    auto src = tmp.path("main.cpp");
    auto other = tmp.path("other.cpp");
    auto add_default_rule = [&](IndexerFixture& f) {
        f.project.config.rules.push_back(
            ConfigRule{.patterns = {"**/other.cpp"}, .default_command = std::string("clang++ -c")});
        f.project.config.finalize(CanonicalPath(Spelling::absolute(tmp.root)));
    };

    {
        IndexerFixture f;
        open_store(tmp, f.project);
        f.project.cdb.add_command(tmp.root, src, llvm::StringRef("clang++ -c main.cpp"));
        add_default_rule(f);
        f.mark_failed(f.project.file_table.intern(Spelling::absolute(other)));
        // Something dirty so the save writes at all; the snapshot rides
        // the same batch.
        auto indexed = index_file(tmp, src);
        ZASSERT(!indexed.data.empty());
        ZASSERT(f.merge(indexed.data));
        f.save();
    }

    IndexerFixture f;
    open_store(tmp, f.project);
    f.project.cdb.add_command(tmp.root, src, llvm::StringRef("clang++ -c main.cpp"));
    add_default_rule(f);
    f.load();
    ZASSERT(f.pump.pending_reason(f.project.file_table.intern(Spelling::absolute(other))) ==
            ReindexReason::ContentChanged);
}

ZEST_CASE(LateDebtShutdownRetry) {
    // Debt showing up after the save serialized its snapshot (write-time
    // corruption recovery) comes back with snapshot_stale set; the
    // shutdown's one metadata retry must commit it to the fresh database,
    // or a dropped unit's repair debt is lost for good.
    struct CorruptOnWrite final : ForwardingDatabase {
        using ForwardingDatabase::ForwardingDatabase;

        bool poisoned = false;

        bool write(llvm::ArrayRef<Blob>, llvm::ArrayRef<index::BlobKey>) override {
            poisoned = true;
            return false;
        }

        bool corrupted() const override {
            return poisoned;
        }
    };

    IndexerFixture f;
    TempDir tmp;
    tmp.touch("other.cpp", "int other() { return 0; }\n");
    tmp.touch("main.cpp", "int use() { return 0; }\n");
    open_store(tmp, f.project);
    f.project.config.rules.push_back(
        ConfigRule{.patterns = {"**/other.cpp"}, .default_command = std::string("clang++ -c")});
    f.project.config.finalize(CanonicalPath(Spelling::absolute(tmp.root)));
    f.project.database = std::make_unique<CorruptOnWrite>(std::move(f.project.database));

    f.mark_failed(f.project.file_table.intern(Spelling::absolute(tmp.path("other.cpp"))));
    auto indexed = index_file(tmp, tmp.path("main.cpp"));
    ZASSERT(!indexed.data.empty());
    ZASSERT(f.merge(indexed.data));

    index::IndexReport report;
    auto body = [&]() -> kota::task<> {
        report = co_await save_project(f.project, f.loop, f.pump.save_debt());
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
    ZASSERT(llvm::StringRef(blob.buffer->getBuffer()).contains("other.cpp"));
}

};  // ZEST_SUITE(CommandSnapshot)

}  // namespace
}  // namespace clice::testing

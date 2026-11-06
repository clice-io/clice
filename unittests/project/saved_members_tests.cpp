module clice;

import :index.database;
import :index.project_index;
import :project.background;
import :project.saved;
import :settings.rules;
import :settings.settings;
import :support.bytes;
import :unittests.utils;
import :unittests.utils.cdb_helper;
import :unittests.utils.forwarding_database;
import :unittests.utils.index_internals;
import :unittests.utils.indexer_fixture;
import :unittests.utils.temp_dir;
import :vfs.path;

namespace clice::testing {
namespace {

ZEST_SUITE(SavedMembers) {

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
    // see it, the identity its manifest records does. The index keeps
    // serving until the reindex merges.
    IndexerFixture f;
    open_store(tmp, f.project);
    f.project.cdb.add_command(tmp.root, src, llvm::StringRef("clang++ -DFOO=2 -c main.cpp"));
    f.load();

    auto tu_id = f.project.file_table.intern(Spelling::absolute(src));
    ZASSERT(f.project.index.indexed(tu_id));
    ZASSERT(f.queue.pending_reason(tu_id) == ReindexReason::ContentChanged);
    // What its index walked under the old command no other unit may skip.
    ZEXPECT(IndexInternals::outdated(f.project.index, tu_id));
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
    ZASSERT(!f.queue.pending_reason(tu_id).has_value());
    ZEXPECT(!IndexInternals::outdated(f.project.index, tu_id));
}

ZEST_CASE(VanishedDatabaseKeepsIndex) {
    TempDir tmp;
    tmp.touch("main.cpp", "int value() { return 1; }\n");
    auto src = tmp.path("main.cpp");
    tmp.touch("compile_commands.json",
              std::format(R"([{{"directory": "{}", "file": "main.cpp", )"
                          R"("arguments": ["clang++", "-c", "main.cpp"]}}])",
                          json_escape(tmp.root)));

    auto open = [&](IndexerFixture& f) {
        open_store(tmp, f.project);
        f.project.root = CanonicalPath(Spelling::absolute(tmp.root));
        f.project.settings.finalize(f.project.root);
        f.project.rules = Rules(f.project.settings, f.project.root, "");
    };

    {
        IndexerFixture f;
        open(f);
        f.project.cdb.load(tmp.path("compile_commands.json"));
        auto indexed = index_file(tmp, src);
        ZASSERT(!indexed.data.empty());
        ZASSERT(f.merge(indexed.data));
        f.save();
    }

    // Discovery no longer finds the database that listed the file, and
    // nothing replaced it: it may come back, and the last-known index
    // keeps serving, as for a database that failed to load.
    fs::remove(tmp.path("compile_commands.json"));
    IndexerFixture f;
    open(f);
    f.load();

    auto tu_id = f.project.file_table.intern(Spelling::absolute(src));
    ZASSERT(f.project.index.indexed(tu_id));
    ZASSERT(!f.queue.pending_reason(tu_id).has_value());
}

ZEST_CASE(NestedDatabaseDropsIndex) {
    TempDir tmp;
    tmp.touch("sub/a.cpp", "int value() { return 1; }\n");
    auto src = tmp.path("sub/a.cpp");
    tmp.touch("sub/compile_commands.json",
              std::format(R"([{{"directory": "{}", "file": "a.cpp", )"
                          R"("arguments": ["clang++", "-c", "a.cpp"]}}])",
                          json_escape(tmp.path("sub"))));
    auto open = [&](IndexerFixture& f) {
        open_store(tmp, f.project);
        f.project.root = CanonicalPath(Spelling::absolute(tmp.root));
        f.project.settings.finalize(f.project.root);
        f.project.rules = Rules(f.project.settings, f.project.root, "");
    };

    {
        IndexerFixture f;
        open(f);
        f.project.cdb.load(tmp.path("sub/compile_commands.json"));
        auto indexed = index_file(tmp, src);
        ZASSERT(!indexed.data.empty());
        ZASSERT(f.merge(indexed.data));
        f.save();
    }

    // sub became a project of its own: the database the last session
    // remembers is gone on purpose, unlike a vanished one, and its units
    // leave this project's index.
    tmp.touch("sub/clice.toml", "");
    IndexerFixture f;
    open(f);
    f.load();

    ZEXPECT(!f.project.index.indexed(f.project.file_table.intern(Spelling::absolute(src))));
}

ZEST_CASE(UndeclaredSourceDropsIndex) {
    TempDir tmp;
    tmp.touch("main.cpp", "int value() { return 1; }\n");
    auto src = tmp.path("main.cpp");
    tmp.touch("a/compile_commands.json", "[]");
    tmp.touch("b/compile_commands.json",
              std::format(R"([{{"directory": "{}", "file": "main.cpp", )"
                          R"("arguments": ["clang++", "-c", "main.cpp"]}}])",
                          json_escape(tmp.root)));
    auto load_declared = [&](IndexerFixture& f, std::vector<std::string> databases) {
        Rule rule;
        rule.compile_commands.assign(databases.begin(), databases.end());
        f.project.settings.rules = {std::move(rule)};
        f.project.root = CanonicalPath(Spelling::absolute(tmp.root));
        f.project.settings.finalize(f.project.root);
        f.project.rules = Rules(f.project.settings, f.project.root, "");
        for(auto source: f.project.rules.declared_databases()) {
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
    ZASSERT(!f.queue.pending_reason(tu_id).has_value());
}

ZEST_CASE(SourceRelocationPersists) {
    TempDir tmp;
    tmp.touch("main.cpp", "int value() { return 1; }\n");
    auto src = tmp.path("main.cpp");
    auto listing = std::format(R"([{{"directory": "{}", "file": "main.cpp", )"
                               R"("arguments": ["clang++", "-c", "main.cpp"]}}])",
                               json_escape(tmp.root));
    auto load_declared = [&](IndexerFixture& f) {
        Rule rule;
        rule.compile_commands.assign({"a", "b"});
        f.project.settings.rules = {std::move(rule)};
        f.project.root = CanonicalPath(Spelling::absolute(tmp.root));
        f.project.settings.finalize(f.project.root);
        f.project.rules = Rules(f.project.settings, f.project.root, "");
        for(auto source: f.project.rules.declared_databases()) {
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
    // index stays, and the members record the database that now
    // guarantees it.
    tmp.touch("a/compile_commands.json", "[]");
    tmp.touch("b/compile_commands.json", listing);
    IndexerFixture f;
    open_store(tmp, f.project);
    load_declared(f);
    f.load();

    auto tu_id = f.project.file_table.intern(Spelling::absolute(src));
    ZASSERT(f.project.index.indexed(tu_id));
    ZASSERT(!f.queue.pending_reason(tu_id).has_value());
    ZASSERT(f.project.saved_members.owes_save(f.project.provisional.changes()));
    f.save();
    ZASSERT(f.project.database->advance_read_snapshot());
    SavedMembers remembered;
    remembered.read(*f.project.database);
    auto databases = remembered.databases(f.project.index);
    ZASSERT(databases.size() == 1);
    ZEXPECT(llvm::StringRef(databases.front().str()).ends_with("b/compile_commands.json"));
}

ZEST_CASE(DiscoveredRelocationDropsIndex) {
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
        f.project.root = CanonicalPath(Spelling::absolute(tmp.root));
        f.project.settings.finalize(f.project.root);
        f.project.rules = Rules(f.project.settings, f.project.root, "");
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
    f.project.root = CanonicalPath(Spelling::absolute(tmp.root));
    f.project.settings.finalize(f.project.root);
    f.project.rules = Rules(f.project.settings, f.project.root, "");
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
        f.project.settings.rules.push_back(Rule{.default_command = std::string("clang++ -c")});
        f.project.root = CanonicalPath(Spelling::absolute(tmp.root));
        f.project.settings.finalize(f.project.root);
        f.project.rules = Rules(f.project.settings, f.project.root, "");
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
    ZASSERT(!f.queue.pending_reason(tu_id).has_value());
}

ZEST_CASE(UncoveredDefaultDropsIndex) {
    TempDir tmp;
    tmp.touch("main.cpp", "int value() { return 1; }\n");
    auto src = tmp.path("main.cpp");

    {
        IndexerFixture f;
        open_store(tmp, f.project);
        f.project.settings.rules.push_back(Rule{.default_command = std::string("clang++ -c")});
        f.project.root = CanonicalPath(Spelling::absolute(tmp.root));
        f.project.settings.finalize(f.project.root);
        f.project.rules = Rules(f.project.settings, f.project.root, "");
        auto indexed = index_file(tmp, src);
        ZASSERT(!indexed.data.empty());
        ZASSERT(f.merge(indexed.data));
        f.save();
    }

    // The rule whose default command covered the file is gone and nothing
    // else compiles it: the index leaves instead of being rebuilt under the
    // builtin fallback, and nothing is owed for a unit the build left.
    IndexerFixture f;
    open_store(tmp, f.project);
    f.load();

    auto tu_id = f.project.file_table.intern(Spelling::absolute(src));
    ZASSERT(!f.project.index.indexed(tu_id));
    ZASSERT(!f.queue.pending_reason(tu_id).has_value());
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
    f.project.settings.rules.push_back(Rule{.patterns = {"**/*.cpp"}, .index = false});
    f.project.root = CanonicalPath(Spelling::absolute(tmp.root));
    f.project.settings.finalize(f.project.root);
    f.project.rules = Rules(f.project.settings, f.project.root, "");
    f.load();

    auto tu_id = f.project.file_table.intern(Spelling::absolute(src));
    ZASSERT(!f.project.index.indexed(tu_id));
    ZASSERT(!f.queue.pending_reason(tu_id).has_value());
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
    // edit, so the recorded identity covers it too.
    IndexerFixture f;
    open_store(tmp, f.project);
    f.project.cdb.add_command(tmp.root, src, llvm::StringRef("clang++ -c main.cpp"));
    f.project.settings.rules.push_back(Rule{
        .patterns = {"**/*.cpp"},
        .append = {"-DFOO=1"},
    });
    f.project.root = CanonicalPath(Spelling::absolute(tmp.root));
    f.project.settings.finalize(f.project.root);
    f.project.rules = Rules(f.project.settings, f.project.root, "");
    f.load();

    auto tu_id = f.project.file_table.intern(Spelling::absolute(src));
    ZASSERT(f.project.index.indexed(tu_id));
    ZASSERT(f.queue.pending_reason(tu_id) == ReindexReason::ContentChanged);
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
    ZASSERT(!f.queue.pending_reason(header_id).has_value());
}

ZEST_CASE(UnitRuleChangeReindexed) {
    TempDir tmp;
    tmp.touch("main.cpp", "int value() { return 1; }\n");
    auto src = tmp.path("main.cpp");

    {
        IndexerFixture f;
        open_store(tmp, f.project);
        f.project.settings.rules.push_back(Rule{.default_command = std::string("clang++ -c")});
        f.project.root = CanonicalPath(Spelling::absolute(tmp.root));
        f.project.settings.finalize(f.project.root);
        f.project.rules = Rules(f.project.settings, f.project.root, "");
        auto indexed = index_file(tmp, src);
        ZASSERT(!indexed.data.empty());
        ZASSERT(f.merge(indexed.data));
        f.save();
    }

    // A clice.toml rule matching the unit changed offline: the unit has no
    // CDB entry, and the identity covers its rules all the same.
    IndexerFixture f;
    open_store(tmp, f.project);
    f.project.settings.rules.push_back(Rule{.default_command = std::string("clang++ -c")});
    f.project.settings.rules.push_back(Rule{
        .patterns = {"**/*.cpp"},
        .append = {"-DFOO=1"},
    });
    f.project.root = CanonicalPath(Spelling::absolute(tmp.root));
    f.project.settings.finalize(f.project.root);
    f.project.rules = Rules(f.project.settings, f.project.root, "");
    f.load();

    auto tu_id = f.project.file_table.intern(Spelling::absolute(src));
    ZASSERT(f.project.index.indexed(tu_id));
    ZASSERT(f.queue.pending_reason(tu_id) == ReindexReason::ContentChanged);
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

    // The members ride the batch with the index state they describe: the
    // failed batch takes both, and both stay owed until a save commits them
    // (`clice index` fails on unsaved state after the final save).
    ZASSERT(!storage->real->contains(index::BlobKind::Listings, "cdb"));
    ZASSERT(f.project.has_unsaved_state());
    storage->fail = false;
    f.save();
    ZASSERT(storage->real->contains(index::BlobKind::Listings, "cdb"));
    ZASSERT(!f.project.has_unsaved_state());
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
            {{index::BlobKind::Listings, "cdb"}});
    }

    // A rerun that dirties nothing must still recreate the members —
    // without it, a later session could not tell a unit the build stopped
    // compiling from one whose database failed to load.
    IndexerFixture f;
    open_store(tmp, f.project);
    f.project.cdb.add_command(tmp.root, src, llvm::StringRef("clang++ -c main.cpp"));
    f.load();
    ZASSERT(f.project.has_unsaved_state());
    f.save();
    ZASSERT(f.project.database->contains(index::BlobKind::Listings, "cdb"));
    ZASSERT(!f.project.has_unsaved_state());
}

ZEST_CASE(SnapshotHoldsSourcesOnly) {
    // The members record which databases listed each file and the
    // provisional members, nothing of the commands; one an older clice
    // wrote, with each file's command hashes, still reads.
    TempDir tmp;
    tmp.touch("main.cpp", "int value() { return 1; }\n");
    tmp.touch("new.cpp", "int fresh() { return 2; }\n");
    tmp.touch("compile_commands.json",
              std::format(R"([{{"directory": "{}", "file": "main.cpp", )"
                          R"("arguments": ["clang++", "-c", "main.cpp"]}}])",
                          json_escape(tmp.root)));
    IndexerFixture f;
    open_store(tmp, f.project);
    f.project.root = CanonicalPath(Spelling::absolute(tmp.root));
    f.project.settings.finalize(f.project.root);
    f.project.rules = Rules(f.project.settings, f.project.root, "");
    f.project.cdb.load(tmp.path("compile_commands.json"));
    ZASSERT(f.project.record_provisional(
        f.project.file_table.intern(Spelling::absolute(tmp.path("new.cpp")))));
    auto indexed = index_file(tmp, tmp.path("main.cpp"));
    ZASSERT(!indexed.data.empty());
    ZASSERT(f.merge(indexed.data));
    f.save();

    auto blob = f.project.database->read(index::BlobKind::Listings, "cdb");
    ZASSERT(bool(blob));
    auto bytes = blob.buffer->getBuffer().str();
    ZEXPECT(llvm::StringRef(bytes).contains("compile_commands.json"));
    ZEXPECT(!llvm::StringRef(bytes).contains("hashes"));

    auto file = std::string_view(R"({"file":)");
    auto pos = bytes.find(file);
    ZASSERT(pos != std::string::npos);
    bytes.replace(
        pos,
        file.size(),
        R"({"hashes":["0123456789abcdef"],"selected":"0123456789abcdef","rules":"","file":)");
    index::BlobDatabase::Blob older{index::BlobKind::Listings, "cdb", copy_bytes(bytes)};
    ZASSERT(f.project.database->write(older, {}));
    ZASSERT(f.project.database->advance_read_snapshot());
    SavedMembers remembered;
    remembered.read(*f.project.database);
    auto databases = remembered.databases(f.project.index);
    ZASSERT(databases.size() == 1u);
    ZEXPECT(llvm::StringRef(databases.front().str()).ends_with("compile_commands.json"));
    auto provisional = remembered.provisional(f.project.index);
    ZASSERT(provisional.size() == 1u);
    ZEXPECT(llvm::StringRef(provisional.front().str()).ends_with("new.cpp"));
}

ZEST_CASE(OwedReindexOutlivesSession) {
    TempDir tmp;
    tmp.touch("main.cpp", "int value() { return 1; }\n");
    auto src = tmp.path("main.cpp");
    auto add_default_rule = [&](IndexerFixture& f, std::string command) {
        f.project.settings.rules.push_back(Rule{.default_command = std::move(command)});
        f.project.root = CanonicalPath(Spelling::absolute(tmp.root));
        f.project.settings.finalize(f.project.root);
        f.project.rules = Rules(f.project.settings, f.project.root, "");
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
        // The default command changed offline, and the reindex it owes
        // never runs this session.
        IndexerFixture f;
        open_store(tmp, f.project);
        add_default_rule(f, "clang++ -DFOO=2 -c");
        f.load();
        auto tu_id = f.project.file_table.intern(Spelling::absolute(src));
        ZASSERT(f.queue.pending_reason(tu_id) == ReindexReason::ContentChanged);
        f.save();
    }

    // The manifest still names the old command: the next session owes the
    // reindex again.
    IndexerFixture f;
    open_store(tmp, f.project);
    add_default_rule(f, "clang++ -DFOO=2 -c");
    f.load();

    auto tu_id = f.project.file_table.intern(Spelling::absolute(src));
    ZASSERT(f.project.index.indexed(tu_id));
    ZASSERT(f.queue.pending_reason(tu_id) == ReindexReason::ContentChanged);
}

ZEST_CASE(DeletedUnitOwesNothing) {
    TempDir tmp;
    tmp.touch("main.cpp", "int value() { return 1; }\n");
    auto src = tmp.path("main.cpp");
    auto add_default_rule = [&](IndexerFixture& f, std::string command) {
        f.project.settings.rules.push_back(Rule{.default_command = std::move(command)});
        f.project.root = CanonicalPath(Spelling::absolute(tmp.root));
        f.project.settings.finalize(f.project.root);
        f.project.rules = Rules(f.project.settings, f.project.root, "");
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

    // The unit was deleted and its command changed while no server ran: a
    // reindex could never succeed, so none is owed, or every later run
    // would stay partial.
    ZASSERT(fs::remove(src));
    IndexerFixture f;
    open_store(tmp, f.project);
    add_default_rule(f, "clang++ -DFOO=2 -c");
    f.load();
    ZASSERT(
        !f.queue.pending_reason(f.project.file_table.intern(Spelling::absolute(src))).has_value());
}

ZEST_CASE(CorruptWriteSavesSnapshot) {
    // A save whose write finds the database corrupt reopens a fresh one and
    // owes it a save; the retry commits the members there too, or the
    // next session could not tell a removed unit.
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
    tmp.touch("main.cpp", "int use() { return 0; }\n");
    tmp.touch("compile_commands.json",
              std::format(R"([{{"directory": "{}", "file": "main.cpp", )"
                          R"("arguments": ["clang++", "-c", "main.cpp"]}}])",
                          json_escape(tmp.root)));
    open_store(tmp, f.project);
    f.project.root = CanonicalPath(Spelling::absolute(tmp.root));
    f.project.settings.finalize(f.project.root);
    f.project.rules = Rules(f.project.settings, f.project.root, "");
    f.project.cdb.load(tmp.path("compile_commands.json"));
    f.project.database = std::make_unique<CorruptOnWrite>(std::move(f.project.database));

    auto indexed = index_file(tmp, tmp.path("main.cpp"));
    ZASSERT(!indexed.data.empty());
    ZASSERT(f.merge(indexed.data));

    index::IndexReport report;
    auto body = [&]() -> async::task<> {
        report = co_await f.project.write_database();
    };
    auto task = body();
    f.loop.schedule(task);
    f.loop.run();

    ZASSERT(report.owes_retry);
    f.project.take_report(report);

    f.save();
    auto blob = f.project.database->read(index::BlobKind::Listings, "cdb");
    ZASSERT(bool(blob));
    ZASSERT(llvm::StringRef(blob.buffer->getBuffer()).contains("compile_commands.json"));
}

};  // ZEST_SUITE(SavedMembers)

}  // namespace
}  // namespace clice::testing

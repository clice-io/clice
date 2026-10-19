module;

#include "modules/prelude.h"

module clice;

import :config.config;
import :index.database;
import :index.project_index;
import :project.save;
import :sched.index.pending_table;
import :unittests.utils.cdb_helper;
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
    ZASSERT(!f.project.index.has_manifest(tu_id));
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
    ZASSERT(f.project.index.has_manifest(tu_id));
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
    ZASSERT(f.project.index.has_manifest(tu_id));
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
    ZASSERT(!f.project.index.has_manifest(tu_id));
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
        ZASSERT(f.merge(indexed.data));
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
    ZASSERT(!f.project.index.has_manifest(tu_id));
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
    ZASSERT(f.project.index.has_manifest(tu_id));
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
    ZASSERT(!f.project.index.has_manifest(tu_id));
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
    ZASSERT(!f.project.index.has_manifest(tu_id));
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
    ZASSERT(!f.project.index.has_manifest(tu_id));
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
        ZASSERT(f.merge(indexed.data));
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

    ZASSERT(!f.project.index.has_manifest(header_id));
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
        ZASSERT(f.merge(indexed.data));
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
    ZASSERT(!f.project.index.has_manifest(header_id));
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
        ZASSERT(f.merge(indexed.data));
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
    ZASSERT(!f.project.index.has_manifest(header_id));
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
        ZASSERT(f.merge(indexed.data));
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

    ZASSERT(!f.project.index.has_manifest(header_id));
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
        ZASSERT(f.merge(indexed.data));
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

    ZASSERT(f.project.index.has_manifest(header_id));
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
        ZASSERT(f.merge(indexed.data));
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
    ZASSERT(f.project.index.has_manifest(header_id));
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

        llvm::SmallVector<std::uint32_t> write(llvm::ArrayRef<Blob> puts,
                                               llvm::ArrayRef<index::BlobKey> removes) override {
            llvm::SmallVector<std::uint32_t> failed;
            for(std::uint32_t i = 0; i < puts.size(); i += 1) {
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
    f.project.index.attach(f.project.database.get());

    auto indexed = index_file(tmp, src);
    ZASSERT(!indexed.data.empty());
    ZASSERT(f.merge(indexed.data));
    f.save();

    // The snapshot rides the batch behind the index state it describes.
    ZASSERT(int(storage->written.back()) == int(index::BlobKind::CommandSnapshot));
    ZASSERT(!storage->real->contains(index::BlobKind::CommandSnapshot, "cdb"));

    // Nothing else is dirty any more, yet the failed snapshot alone must
    // drive the next save until it commits — and until it does, the state
    // counts as unsaved (`clice index` fails on it after the final save).
    ZASSERT(has_unsaved_state(f.project));
    storage->fail_cdb = false;
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
    ZASSERT(has_unsaved_state(f.project));
    f.save();
    ZASSERT(f.project.database->contains(index::BlobKind::CommandSnapshot, "cdb"));
    ZASSERT(!has_unsaved_state(f.project));
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
        ZASSERT(f.merge(indexed.data));
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
        ZASSERT(!f.project.index.has_manifest(header_id));
        ZASSERT(f.pump.pending_reason(header_id) == ReindexReason::ContentChanged);
        f.save();
    }

    IndexerFixture f;
    open_store(tmp, f.project);
    f.project.cdb.add_command(tmp.root, src, llvm::StringRef("clang++ -DFOO=2 -c main.cpp"));
    f.load();

    auto header_id = f.project.file_table.intern(Spelling::absolute(header));
    ZASSERT(!f.project.index.has_manifest(header_id));
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
        ZASSERT(f.merge(indexed.data));
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
        ZASSERT(f.merge(indexed.data));
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

        llvm::SmallVector<std::uint32_t> write(llvm::ArrayRef<Blob> puts,
                                               llvm::ArrayRef<index::BlobKey>) override {
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
    ZASSERT(f.merge(indexed.data));

    index::IndexReport report;
    auto body = [&]() -> kota::task<> {
        report = co_await save_project(f.project, f.commands, f.loop, f.pump.save_debt());
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

};  // ZEST_SUITE(CommandSnapshot)

}  // namespace
}  // namespace clice::testing

module clice;

import :index.database.writer_lock;
import :project.discovery;
import :project.project;
import :settings.settings;
import :unittests.utils.cdb_helper;
import :unittests.utils.temp_dir;
import :unittests.utils;
import :vfs.cache_store;
import :vfs.file_table;
import :vfs.path;

namespace clice::testing {
namespace {

ZEST_SUITE(ProjectOpen) {

/// A project of one unit including a header, its database at the root.
TempDir tmp;
CanonicalPath root{Spelling::absolute(tmp.root)};
FileTable files;
Project project{files};

ProjectOpenTEST() {
    tmp.touch("main.cpp", "#include \"util.h\"\nint main() { return util(); }\n");
    tmp.touch("util.h", "inline int util() { return 0; }\n");
    tmp.touch("compile_commands.json",
              build_cdb_json({
                  {.dir = tmp.root, .file = tmp.path("main.cpp")}
    }));
}

/// Open `into` at the root under its settings, as `options` say.
Project::Opened open(Project& into, Project::OpenOptions options) {
    auto loaded = load_settings(root);
    ZASSERT(!loaded.broken());
    into.root = root;
    into.settings = std::move(loaded.settings);
    options.configuration = loaded.configuration;
    return into.open(options);
}

Fid fid(llvm::StringRef relative) {
    return files.intern(Spelling::absolute(tmp.path(relative)));
}

void cache_blob(llvm::StringRef name, std::uint64_t size) {
    auto relative = std::format(".clice/cache/v{}/{}", cache_format_version, name);
    tmp.touch(relative);
    auto file = fs::File::open(tmp.path(relative), {.read = true, .write = true});
    ZASSERT(file.has_value());
    // Sparse files exercise GiB budgets without writing or mapping GiB of data.
    file->set_sparse();
#ifdef _WIN32
    auto sparse = file->is_sparse();
    ZASSERT(sparse.has_value());
    ZASSERT(*sparse);
#endif
    ZASSERT(!file->resize(size));
}

ZEST_CASE(DefaultCacheBudgets) {
    constexpr std::uint64_t budget = 8ull << 30;
    for(auto ns: {"pch", "pcm"}) {
        cache_blob(std::format("{}/a.{}", ns, ns), budget / 2);
        cache_blob(std::format("{}/b.{}", ns, ns), budget / 2);
    }
    auto opened = open(
        project,
        {.load = Project::OpenOptions::Load::Rules, .cache = Project::OpenOptions::Cache::Build});
    ZEXPECT(opened.unwritable.empty());
    ZASSERT(project.store.has_value());
    ZEXPECT(project.settings.cache.pch_budget_bytes == budget);
    ZEXPECT(project.settings.cache.pcm_budget_bytes == budget);
    for(auto ns: {"pch", "pcm"}) {
        ZASSERT(project.store->lookup(ns, "a").has_value());
        ZASSERT(project.store->lookup(ns, "b").has_value());
        auto pending = project.store->begin_store(ns, "c");
        ZASSERT(!fs::write_file(pending.tmp_path, "x"));
        auto committed = project.store->commit(std::move(pending));
        ZASSERT(committed.has_value());
        ZEXPECT(project.store->lookup(ns, "c").has_value());
        auto evicted = project.store->take_evictions();
        ZASSERT(evicted.size() == 1U);
        ZEXPECT(evicted.front().ns == std::string(ns));
    }
}

ZEST_CASE(ConfiguredCacheBudgets) {
    tmp.touch("clice.toml", R"(
[cache]
pch_budget_bytes = 10
pcm_budget_bytes = 20
)");
    for(auto key: {"a", "b"}) {
        cache_blob(std::format("pch/{}.pch", key), 4);
        cache_blob(std::format("pch/{}.pch.idx", key), 3);
    }
    for(auto key: {"a", "b", "c"}) {
        cache_blob(std::format("pcm/{}.pcm", key), 7);
    }
    auto opened = open(
        project,
        {.load = Project::OpenOptions::Load::Rules, .cache = Project::OpenOptions::Cache::Build});
    ZEXPECT(opened.unwritable.empty());
    ZASSERT(project.store.has_value());
    auto a = project.store->lookup("pch", "a");
    auto b = project.store->lookup("pch", "b");
    ZEXPECT(a.has_value() != b.has_value());
    ZEXPECT(project.store->lookup_aux("pch", "a").has_value() == a.has_value());
    ZEXPECT(project.store->lookup_aux("pch", "b").has_value() == b.has_value());
    std::uint32_t pcms = 0;
    for(auto key: {"a", "b", "c"}) {
        pcms += project.store->lookup("pcm", key).has_value();
    }
    ZEXPECT(pcms == 2U);
}

ZEST_CASE(LargeCacheBudgets) {
    constexpr std::uint64_t pch_budget = 10ull << 30;
    constexpr std::uint64_t pcm_budget = 12ull << 30;
    tmp.touch("clice.toml",
              std::format("[cache]\npch_budget_bytes = {}\npcm_budget_bytes = {}\n",
                          pch_budget,
                          pcm_budget));
    for(auto key: {"a", "b"}) {
        cache_blob(std::format("pch/{}.pch", key), pch_budget / 2);
        cache_blob(std::format("pcm/{}.pcm", key), pcm_budget / 2);
    }
    auto opened = open(
        project,
        {.load = Project::OpenOptions::Load::Rules, .cache = Project::OpenOptions::Cache::Build});
    ZEXPECT(opened.unwritable.empty());
    ZASSERT(project.store.has_value());
    for(auto ns: {"pch", "pcm"}) {
        ZEXPECT(project.store->lookup(ns, "a").has_value());
        ZEXPECT(project.store->lookup(ns, "b").has_value());
    }
}

ZEST_CASE(ReadOnlyCacheBudgets) {
    cache_blob("pch/a.pch", 4);
    cache_blob("pch/a.pch.idx", 3);
    cache_blob("pcm/a.pcm", 7);
    auto written = open(project, {.cache = Project::OpenOptions::Cache::Write});
    ZEXPECT(written.unwritable.empty());
    ZASSERT(project.store.has_value());
    ZASSERT(project.database != nullptr);
    ZASSERT(project.store->lookup("pch", "a").has_value());
    ZASSERT(project.store->lookup("pcm", "a").has_value());
    project.store->checkpoint();
    auto manifest = path::join(project.store->base_dir(), "manifest.json");
    auto before = read_file(manifest);
    ZASSERT(before.has_value());
    tmp.touch("clice.toml", R"(
[cache]
pch_budget_bytes = 1
pcm_budget_bytes = 1
)");
    FileTable reader_files;
    Project reader{reader_files};
    auto read = open(reader, {.cache = Project::OpenOptions::Cache::Read});
    ZEXPECT(read.unreadable.empty());
    ZASSERT(reader.store.has_value());
    ZEXPECT(reader.store->read_only());
    ZEXPECT(reader.settings.cache.pch_budget_bytes == 1U);
    ZEXPECT(reader.settings.cache.pcm_budget_bytes == 1U);
    for(auto ns: {"pch", "pcm"}) {
        auto hit = reader.store->lookup(ns, "a");
        ZASSERT(hit.has_value());
        ZEXPECT(fs::exists(*hit));
    }
    ZEXPECT(reader.store->lookup_aux("pch", "a").has_value());
    reader.store->checkpoint();
    reader.store->shutdown();
    ZEXPECT(read_file(manifest).value_or("") == *before);
}

ZEST_CASE(RulesWithoutDatabases) {
    tmp.touch("clice.toml", R"(
default_configuration = "debug"

[[rules]]
configuration = "debug"
append = ["-DDEBUG"]
)");
    auto opened = open(project, {.load = Project::OpenOptions::Load::Rules});
    ZEXPECT(project.rules.configuration() == "debug");
    ZEXPECT(project.cdb.source_count() == 0U);
    ZEXPECT(opened.members.empty());
}

ZEST_CASE(DatabasesWithoutGraph) {
    auto opened = open(project, {.load = Project::OpenOptions::Load::Databases});
    ZEXPECT(project.cdb.source_count() == 1U);
    ZEXPECT(!project.entries(fid("main.cpp")).empty());
    ZEXPECT(opened.members.empty());
    ZEXPECT(!project.dep_graph.knows(fid("util.h")));
}

ZEST_CASE(MembersScanGraph) {
    auto opened = open(project, {});
    ZEXPECT(opened.members.size() == 1U);
    ZEXPECT(project.dep_graph.knows(fid("util.h")));
}

ZEST_CASE(DatabaseAboveFile) {
    // Below the root's direct subdirectories, discovery finds a database
    // only above the file a run is about.
    tmp.touch("deep/er/tool.cpp", "int tool() { return 1; }\n");
    tmp.touch("deep/er/compile_commands.json",
              build_cdb_json({
                  {.dir = tmp.root, .file = tmp.path("deep/er/tool.cpp")}
    }));
    open(project,
         {
             .load = Project::OpenOptions::Load::Databases,
             .files = {CanonicalPath(Spelling::absolute(tmp.path("deep/er/tool.cpp")))},
         });
    ZEXPECT(project.cdb.source_count() == 2U);
    ZEXPECT(!project.entries(fid("deep/er/tool.cpp")).empty());
}

ZEST_CASE(ReaderWithoutIndex) {
    auto opened = open(project,
                       {
                           .cache = Project::OpenOptions::Cache::Read,
                       });
    ZEXPECT(llvm::StringRef(opened.unreadable).starts_with("No index cache at"));
    ZEXPECT(!project.store.has_value());
    ZEXPECT(project.database == nullptr);
    // A reader creates nothing in the cache directory.
    ZEXPECT(!fs::exists(tmp.path(".clice")));
}

ZEST_CASE(WriterThenReader) {
    auto written = open(project,
                        {
                            .cache = Project::OpenOptions::Cache::Write,
                        });
    ZEXPECT(written.unreadable.empty());
    ZASSERT(project.database != nullptr);
    ZEXPECT(!project.database->read_only());
    ZEXPECT(project.writer_lock.has_value());

    // Beside the writer, a reader opens the database read-only, without
    // the lock; an index with no units yet reads as empty with the build.
    FileTable reader_files;
    Project reader{reader_files};
    auto read = open(reader,
                     {
                         .cache = Project::OpenOptions::Cache::Read,
                     });
    ZEXPECT(read.unreadable.empty());
    ZASSERT(reader.database != nullptr);
    ZEXPECT(reader.database->read_only());
    ZEXPECT(!reader.writer_lock.has_value());
}

ZEST_CASE(SecondWriterWaits) {
    // A writer finding the lock held leaves its index closed, taking no
    // build's entries, until the lock comes free: then it opens it as the
    // writer.
    open(project, {.cache = Project::OpenOptions::Cache::Write});
    ZASSERT(project.writer_lock.has_value());

    FileTable second_files;
    Project second{second_files};
    open(second, {.cache = Project::OpenOptions::Cache::Write});
    ZEXPECT(second.index_waits);
    ZEXPECT(second.database == nullptr);

    project.database.reset();
    project.writer_lock.reset();
    auto lock = index::WriterLock::acquire(second.settings.cache_dir);
    ZASSERT(lock.has_value());
    Project::Opened opened;
    second.open_index(std::move(*lock), opened);
    ZEXPECT(!second.index_waits);
    ZEXPECT(opened.unwritable.empty());
    ZASSERT(second.database != nullptr);
    ZEXPECT(!second.database->read_only());
    ZEXPECT(second.writer_lock.has_value());
}

};  // ZEST_SUITE(ProjectOpen)

}  // namespace
}  // namespace clice::testing

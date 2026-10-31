module clice;

import :project.discovery;
import :project.project;
import :settings.settings;
import :unittests.utils.cdb_helper;
import :unittests.utils.temp_dir;
import :unittests.utils;
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
             .file = CanonicalPath(Spelling::absolute(tmp.path("deep/er/tool.cpp"))),
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

};  // ZEST_SUITE(ProjectOpen)

}  // namespace
}  // namespace clice::testing

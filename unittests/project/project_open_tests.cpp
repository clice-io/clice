module clice;

import :config.config;
import :project.project;
import :unittests.utils.cdb_helper;
import :unittests.utils.temp_dir;
import :unittests.utils;
import :vfs.file_table;
import :vfs.path;

namespace clice::testing {
namespace {

ZEST_SUITE(ProjectOpen) {

/// A project of one unit including a header, its database at the root.
struct Layout {
    TempDir tmp;
    CanonicalPath root{Spelling::absolute(tmp.root)};
    FileTable files;
    Project project{files};

    Layout() {
        tmp.touch("main.cpp", "#include \"util.h\"\nint main() { return util(); }\n");
        tmp.touch("util.h", "inline int util() { return 0; }\n");
        tmp.touch("compile_commands.json",
                  build_cdb_json({
                      {.dir = tmp.root, .file = tmp.path("main.cpp")}
        }));
    }

    Project::Opened open(Project::OpenOptions options) {
        auto loaded = load_config(root);
        ZASSERT(!loaded.broken());
        project.config = std::move(loaded.config);
        options.configuration = loaded.configuration;
        return project.open(options);
    }

    Fid fid(llvm::StringRef relative) {
        return files.intern(Spelling::absolute(tmp.path(relative)));
    }
};

ZEST_CASE(RulesWithoutDatabases) {
    Layout layout;
    layout.tmp.touch("clice.toml", R"(
default_configuration = "debug"

[[rules]]
configuration = "debug"
append = ["-DDEBUG"]
)");
    auto opened = layout.open({.build = Project::OpenOptions::Build::None});
    ZEXPECT(layout.project.build.active_configuration() == "debug");
    ZEXPECT(layout.project.cdb.source_count() == 0U);
    ZEXPECT(opened.members.empty());
}

ZEST_CASE(DatabasesWithoutGraph) {
    Layout layout;
    auto opened = layout.open({.build = Project::OpenOptions::Build::Databases});
    ZEXPECT(layout.project.cdb.source_count() == 1U);
    ZEXPECT(!layout.project.build.entries(layout.fid("main.cpp")).empty());
    ZEXPECT(opened.members.empty());
    ZEXPECT(!layout.project.dep_graph.knows(layout.fid("util.h")));
}

ZEST_CASE(MembersScanGraph) {
    Layout layout;
    auto opened = layout.open({});
    ZEXPECT(opened.has_commands);
    ZEXPECT(opened.members.size() == 1U);
    ZEXPECT(layout.project.dep_graph.knows(layout.fid("util.h")));
}

ZEST_CASE(DatabaseAboveFile) {
    // Below the root's direct subdirectories, discovery finds a database
    // only above the file a run is about.
    Layout layout;
    layout.tmp.touch("deep/er/tool.cpp", "int tool() { return 1; }\n");
    layout.tmp.touch("deep/er/compile_commands.json",
                     build_cdb_json({
                         {.dir = layout.tmp.root, .file = layout.tmp.path("deep/er/tool.cpp")}
    }));
    auto tool = layout.tmp.path("deep/er/tool.cpp");
    layout.open({
        .build = Project::OpenOptions::Build::Databases,
        .file = CanonicalPath(Spelling::absolute(tool)),
    });
    ZEXPECT(layout.project.cdb.source_count() == 2U);
    ZEXPECT(!layout.project.build.entries(layout.fid("deep/er/tool.cpp")).empty());
}

ZEST_CASE(ReaderWithoutIndex) {
    Layout layout;
    auto opened = layout.open({
        .store = Project::OpenOptions::Store::ReadOnly,
        .index = Project::OpenOptions::Index::Read,
    });
    ZEXPECT(llvm::StringRef(opened.unreadable).starts_with("No index cache at"));
    ZEXPECT(!layout.project.store.has_value());
    ZEXPECT(layout.project.database == nullptr);
    // A reader creates nothing in the cache directory.
    ZEXPECT(!fs::exists(layout.tmp.path(".clice")));
}

ZEST_CASE(WriterThenReader) {
    Layout layout;
    auto written = layout.open({
        .store = Project::OpenOptions::Store::Writable,
        .index = Project::OpenOptions::Index::Write,
    });
    ZEXPECT(written.unreadable.empty());
    ZASSERT(layout.project.database != nullptr);
    ZEXPECT(!layout.project.database->read_only());
    ZEXPECT(layout.project.writer_lock.has_value());

    // Beside the writer, a reader opens the database read-only, without
    // the lock; an index with no units yet reads as empty with the build.
    FileTable files;
    Project reader{files};
    auto loaded = load_config(layout.root);
    reader.config = std::move(loaded.config);
    auto read = reader.open({
        .configuration = loaded.configuration,
        .store = Project::OpenOptions::Store::ReadOnly,
        .index = Project::OpenOptions::Index::Read,
    });
    ZEXPECT(read.unreadable.empty());
    ZASSERT(reader.database != nullptr);
    ZEXPECT(reader.database->read_only());
    ZEXPECT(!reader.writer_lock.has_value());
}

};  // ZEST_SUITE(ProjectOpen)

}  // namespace
}  // namespace clice::testing

module clice;

import :settings.settings;
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
    auto opened = open(project, {.build = Project::OpenOptions::Build::None});
    ZEXPECT(project.build.active_configuration() == "debug");
    ZEXPECT(project.cdb.source_count() == 0U);
    ZEXPECT(opened.members.empty());
}

ZEST_CASE(DatabasesWithoutGraph) {
    auto opened = open(project, {.build = Project::OpenOptions::Build::Databases});
    ZEXPECT(project.cdb.source_count() == 1U);
    ZEXPECT(!project.build.entries(fid("main.cpp")).empty());
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
             .build = Project::OpenOptions::Build::Databases,
             .file = CanonicalPath(Spelling::absolute(tmp.path("deep/er/tool.cpp"))),
         });
    ZEXPECT(project.cdb.source_count() == 2U);
    ZEXPECT(!project.build.entries(fid("deep/er/tool.cpp")).empty());
}

ZEST_CASE(ReaderWithoutIndex) {
    auto opened = open(project,
                       {
                           .store = Project::OpenOptions::Store::ReadOnly,
                           .index = Project::OpenOptions::Index::Read,
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
                            .store = Project::OpenOptions::Store::Writable,
                            .index = Project::OpenOptions::Index::Write,
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
                         .store = Project::OpenOptions::Store::ReadOnly,
                         .index = Project::OpenOptions::Index::Read,
                     });
    ZEXPECT(read.unreadable.empty());
    ZASSERT(reader.database != nullptr);
    ZEXPECT(reader.database->read_only());
    ZEXPECT(!reader.writer_lock.has_value());
}

};  // ZEST_SUITE(ProjectOpen)

ZEST_SUITE(DatabaseKey) {

ZEST_CASE(Resolved) {
    /// A configuration resolving symlinks keeps a database per real path
    /// its databases resolve to — the own one of a configuration declaring
    /// exactly those — and any other configuration its own.
    TempDir tmp;
    tmp.touch("out/a/compile_commands.json", "[]");
    tmp.touch("out/b/compile_commands.json", "[]");
    auto configured = [&](llvm::StringRef followed) {
        Settings settings;
        settings.rules.push_back(Rule{.configuration = "active",
                                      .compile_commands = {followed.str()},
                                      .resolve_symlinks = true});
        settings.rules.push_back(Rule{.configuration = "a", .compile_commands = {"out/a"}});
        settings.rules.push_back(Rule{
            .configuration = "both",
            .compile_commands = {"out/a", "out/b"}
        });
        settings.finalize(CanonicalPath(Spelling::absolute(tmp.root)));
        return settings;
    };
    auto key_of = [](const Settings& settings, llvm::StringRef configuration) {
        return database_key(settings, configuration, resolved_databases(settings, configuration));
    };
    auto settings = configured("out/a");
    ZEXPECT(key_of(settings, "active").str() == "a");
    ZEXPECT(key_of(settings, "a").str() == "a");
    ZEXPECT(key_of(settings, "both").str() == "both");

    settings = configured("out/b");
    auto key = key_of(settings, "active");
    ZEXPECT(key.configuration == "active");
    ZEXPECT(key.resolved ==
            CanonicalPath(Spelling::absolute(tmp.path("out/b/compile_commands.json"))).str());

    // Naming the databases is not all it does: another build.
    settings = configured("out/a");
    settings.rules[1].append = {"-DTRACE"};
    settings.finalize(CanonicalPath(Spelling::absolute(tmp.root)));
    ZEXPECT(key_of(settings, "active").configuration == "active");
};

};  // ZEST_SUITE(DatabaseKey)

}  // namespace
}  // namespace clice::testing

module clice:unittests.utils.project_fixture;

import :project.background;
import :project.graph.task_graph;
import :project.project;
import :settings.rules;
import :settings.settings;
import :support.prelude;
import :unittests.utils.temp_dir;
import :vfs.cache_store;
import :vfs.file_table;
import :vfs.path;
import :worker.pool;

namespace clice::testing {

/// Finish the settings of `project`, compiling its rules over its root, as
/// loading them does.
inline void finish_settings(Project& project) {
    project.settings.finalize(project.root);
    project.rules = Rules(project.settings, project.root, "");
}

/// Turn the toolchain query off for every file of `project`, then finish its
/// settings: the tests of a fixture calling this are not about the
/// toolchain, and a compiler's first query pays its cold start (seconds on a
/// fresh Windows runner).
inline void skip_toolchain_query(Project& project) {
    project.settings.rules.push_back(Rule{.patterns = {"**"}, .query_toolchain = false});
    finish_settings(project);
}

/// Give `project` a cache store in `tmp` holding its PCHs and PCMs, as a
/// project opening to compile its members registers them.
inline void open_cache(TempDir& tmp, Project& project) {
    auto store = CacheStore::open(tmp.path("cache"), 1);
    ZASSERT(store);
    store->register_namespace({.name = "pch", .extension = ".pch", .aux_extension = ".pch.idx"});
    store->register_namespace({.name = "pcm", .extension = ".pcm"});
    project.store.emplace(std::move(*store));
}

/// A project at `root` with the event loop and the workerless pool it
/// builds with, and the documents its owner has open, handed over as the
/// server hands them (OpenFile).
struct ProjectFixture {
    async::event_loop loop;
    FileTable files;
    WorkerPool pool{loop};
    Project project;
    llvm::SmallVector<OpenFile> open_files;

    explicit ProjectFixture(llvm::StringRef root = "/proj") :
        project(files, CanonicalPath(Spelling::absolute(root)), &loop, &pool) {
        skip_toolchain_query(project);
    }

    Fid intern(llvm::StringRef path) {
        return files.intern(Spelling::absolute(path));
    }

    /// Open `path`, its last compile having read `read`, as the server
    /// adopts a compiled document: its node joins the graph, and the disk
    /// under it is current.
    Fid open_document(llvm::StringRef path, llvm::SmallVector<Fid> read = {}) {
        auto file = intern(path);
        open_files.push_back({.file = file, .read = std::move(read)});
        project.graph.add(ast_node(file));
        files.disk.current(file);
        return file;
    }
};

}  // namespace clice::testing

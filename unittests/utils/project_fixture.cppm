module clice:unittests.utils.project_fixture;

import :project.background;
import :project.graph.task_graph;
import :project.project;
import :settings.rules;
import :support.prelude;
import :vfs.file_table;
import :vfs.path;
import :worker.pool;

namespace clice::testing {

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
        project.rules = Rules(project.settings, project.root, "");
    }

    Fid intern(llvm::StringRef path) {
        return files.intern(Spelling::absolute(path));
    }

    /// Open `path`, its last compile having read `read`, as the server
    /// adopts a compiled document: its node joins the graph, the disk under
    /// it is current, and unless its compile skips the bodies of its
    /// headers, its index is left to it.
    Fid open_document(llvm::StringRef path, llvm::SmallVector<Fid> read = {}) {
        auto file = intern(path);
        open_files.push_back({.file = file, .read = std::move(read)});
        project.graph.add(ast_node(file));
        files.disk.current(file);
        if(!project.settings.project.skip_header_bodies) {
            project.background_index.leave_to_document(file);
        }
        return file;
    }
};

}  // namespace clice::testing

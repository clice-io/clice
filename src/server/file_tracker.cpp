module clice;

import :project.configuration;
import :server.file_tracker;
import :vfs.file_system;
import :vfs.path;

namespace clice {

/// The git directory of the checkout `root` lies in, if any: `.git`
/// itself, or the directory a linked worktree's `.git` file names.
static std::optional<std::string> git_dir(llvm::StringRef root) {
    for(auto dir: path::ancestors(root)) {
        auto git = path::join(dir, ".git");
        auto status = fs::status(git);
        if(!status) {
            continue;
        }
        if(status->type == fs::FileType::Directory) {
            return git;
        }
        if(auto text = vfs::read_text(git)) {
            llvm::StringRef target = *text;
            if(target.consume_front("gitdir:")) {
                return Spelling(target.trim(), Spelling::absolute(dir)).str();
            }
        }
        return std::nullopt;
    }
    return std::nullopt;
}

FileTracker::FileTracker(Project& project, const SessionStore& store, CanonicalPath root) :
    project(project), store(store), cdb(project, root) {
    auto git = git_dir(root);
    if(!git) {
        return;
    }
    for(auto name: {"HEAD", "index"}) {
        checkout.push_back(project.file_table.disk.watch(
            path::join(*git, name),
            [&disk = project.file_table.disk, root = root.str()] { disk.expire_under(root); }));
    }
}

static void push_delta(CDBDiff diff, llvm::SmallVectorImpl<FileEvent>& events) {
    if(!diff.empty()) {
        events.push_back(FileEvent::cdb_changed(std::move(diff)));
    }
}

bool FileTracker::moved() const {
    return resolved_databases(project.config, project.build.active_configuration()) !=
           project.resolved_databases;
}

llvm::SmallVector<FileEvent> FileTracker::tick_cdb(bool force) {
    llvm::SmallVector<Fid> open_files;
    for(auto& [path_id, session]: store.sessions) {
        open_files.push_back(path_id);
    }
    llvm::SmallVector<FileEvent> events;
    push_delta(cdb.tick(open_files, force), events);
    return events;
}

llvm::SmallVector<FileEvent> FileTracker::discover_around(Fid path_id) {
    llvm::SmallVector<FileEvent> events;
    push_delta(cdb.discover_around(path_id), events);
    return events;
}

kota::task<llvm::SmallVector<FileEvent>> FileTracker::tick_sources() {
    auto walked =
        co_await kota::queue([walk = project.build.source_walk()] { return walk_sources(walk); });
    llvm::SmallVector<FileEvent> events;
    push_delta({.added = project.build.refresh_default_sources(walked)}, events);
    co_return events;
}

}  // namespace clice

#include "server/file_tracker.h"

#include <optional>
#include <string>
#include <utility>

#include "vfs/file_system.h"
#include "vfs/path.h"

#include "llvm/Support/FileSystem.h"

namespace clice {

/// The git directory of the checkout `root` lies in, if any: `.git`
/// itself, or the directory a linked worktree's `.git` file names.
static std::optional<std::string> git_dir(llvm::StringRef root) {
    std::optional<std::string> found;
    path::walk_ancestors(root, "", [&](llvm::StringRef dir) {
        auto git = path::join(dir, ".git");
        auto status = vfs::status(git);
        if(!status) {
            return true;
        }
        if(status->type == llvm::sys::fs::file_type::directory_file) {
            found = git;
        } else if(auto text = vfs::read(git)) {
            llvm::StringRef target = (*text)->getBuffer();
            if(target.consume_front("gitdir:")) {
                found = Spelling(target.trim(), Spelling::absolute(dir)).str();
            }
        }
        return false;
    });
    return found;
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

/// Diff ids and event ids share the single file table.
static void push_delta(const CDBDiff& diff, llvm::SmallVectorImpl<FileEvent>& events) {
    if(diff.empty()) {
        return;
    }
    FileEvent::CDBDelta delta;
    delta.added.assign(diff.added.begin(), diff.added.end());
    delta.removed.assign(diff.removed.begin(), diff.removed.end());
    delta.changed.assign(diff.changed.begin(), diff.changed.end());
    events.push_back(FileEvent::cdb_changed(std::move(delta)));
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
    if(walked.has_value()) {
        push_delta({.added = project.build.refresh_default_sources(*walked)}, events);
    }
    co_return events;
}

}  // namespace clice

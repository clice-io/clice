#include "server/file_tracker.h"

#include <algorithm>
#include <chrono>
#include <utility>

#include "support/filesystem.h"
#include "support/logging.h"
#include "support/timer.h"

#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/ScopeExit.h"
#include "llvm/Support/Chrono.h"
#include "llvm/Support/FileSystem.h"

namespace clice {

FileTracker::FileTracker(Project& project, const SessionStore& store, std::string root) :
    project(project), store(store), cdb(project, std::move(root)) {}

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

kota::task<llvm::SmallVector<FileEvent>> FileTracker::tick_workspace() {
    constexpr std::size_t batch_size = 500;

    if(sweeping) {
        // The poll hook can land while the live loop is suspended between
        // batches; the running sweep already covers this request.
        co_return llvm::SmallVector<FileEvent>{};
    }
    sweeping = true;
    auto guard = llvm::make_scope_exit([this] { sweeping = false; });

    ScopedTimer timer;
    // What the lexical scan saw included, and what compiles actually read
    // (the files index rows came from): a header only a macro include
    // reaches is invisible to the former.
    auto files = project.dep_graph.all_files();
    for(auto& [file, contributors]: project.project_index.contributions) {
        files.push_back(file);
    }
    llvm::sort(files);
    files.erase(llvm::unique(files), files.end());
    for(std::size_t begin = 0; begin < files.size(); begin += batch_size) {
        if(begin != 0) {
            // Yield one loop iteration between batches so a long sweep
            // never starves LSP traffic.
            co_await kota::sleep(std::chrono::milliseconds(0));
        }

        auto batch_end = std::min(begin + batch_size, files.size());
        for(std::size_t i = begin; i < batch_end; ++i) {
            auto path_id = files[i];
            llvm::sys::fs::file_status status;
            if(llvm::sys::fs::status(project.file_table.resolve(path_id), status)) {
                project.file_table.saw_missing(path_id);
                continue;
            }
            // Reads only when the shared pair cannot vouch for the stat. A
            // file that stats fine but cannot be read right now (e.g. an
            // antivirus scanner briefly holding a fresh file on Windows)
            // leaves what was seen untouched; the next tick looks again.
            project.file_table.observe_for(path_id, status);
        }
    }

    // A file created under a default-command rule joins the build: the
    // same gain of a command a database reload reports as added. One
    // deleted left through DiskRemoved, like any tracked file.
    llvm::SmallVector<FileEvent> events;
    push_delta({.added = project.build.refresh_default_sources()}, events);

    LOG_PERF("tracker", "phase=workspace_sweep files={} elapsed_ms={}", files.size(), timer.ms());
    co_return events;
}

}  // namespace clice

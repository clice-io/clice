#include "server/workspace/invalidator.h"

#include <utility>

#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/MemoryBuffer.h"

namespace clice {

/// Default ReadFile: the real filesystem.
static std::optional<std::string> read_from_disk(llvm::StringRef path) {
    auto buffer = llvm::MemoryBuffer::getFile(path);
    if(!buffer) {
        return std::nullopt;
    }
    return std::string((*buffer)->getBuffer());
}

Invalidator::Invalidator(Workspace& workspace, const SessionStore& store, ReadFile read_file) :
    workspace(workspace), store(store),
    read_file(read_file ? std::move(read_file) : ReadFile(read_from_disk)) {}

/// Batch effects may name the same file twice (two saves in one batch);
/// execution must see each id once.
static void dedup(llvm::SmallVector<std::uint32_t>& ids) {
    llvm::sort(ids);
    ids.erase(llvm::unique(ids), ids.end());
}

DirtySet Invalidator::apply(llvm::ArrayRef<FileEvent> events) {
    DirtySet dirty;

    for(auto& event: events) {
        switch(event.kind) {
            case FileEvent::Kind::BufferOpened: {
                // Buffer installation itself is SessionStore::apply_open's
                // job; nothing cross-file to invalidate yet.
                break;
            }
            case FileEvent::Kind::BufferEdited: {
                // Buffer sync (text/version/ast_dirty/generation) is
                // SessionStore::apply_change's job; nothing cross-file yet.
                break;
            }
            case FileEvent::Kind::BufferSaved: {
                break;
            }
            case FileEvent::Kind::BufferClosed: {
                break;
            }
            case FileEvent::Kind::DiskChanged: {
                // TODO: no producer yet — the disk poller / cache validation
                // side will emit these.
                break;
            }
            case FileEvent::Kind::DiskRemoved: {
                // TODO: no producer yet (see DiskChanged).
                break;
            }
            case FileEvent::Kind::ContextChanged: {
                break;
            }
            case FileEvent::Kind::WorkerCrashed: {
                break;
            }
        }
    }

    dedup(dirty.mark_ast_dirty);
    dedup(dirty.reset_trial);
    dedup(dirty.force_revalidate);
    dedup(dirty.enqueue_reindex);
    return dirty;
}

}  // namespace clice

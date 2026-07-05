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

Invalidator::Invalidator(Workspace& workspace,
                         const SessionStore& store,
                         const ContextResolver& contexts,
                         ReadFile read_file) :
    workspace(workspace), store(store), contexts(contexts),
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
                auto path_id = event.path_id;

                // The saved file's own self-containment may have changed;
                // re-evaluate on its next compile.
                dirty.reset_header_mode.push_back(path_id);
                dirty.reset_trial.push_back(path_id);

                // Root TUs transitively including the saved file, snapshotted
                // before the rescan rewrites the include graph. A save only
                // rewrites the saved file's own outgoing edges, so this set
                // normally equals the post-rescan one — the pre-rescan
                // snapshot is a cheap safety net for a reverse map that was
                // stale at save time.
                auto old_dependents = workspace.dep_graph.find_host_sources(path_id);

                // Rescan disk state (include edges, module declaration,
                // compile-graph cascade, PCM caches); the cascade names the
                // module units whose build products went stale.
                auto dirtied = workspace.rescan_after_save(path_id);
                for(auto dirty_id: dirtied) {
                    if(store.find(dirty_id)) {
                        dirty.mark_ast_dirty.push_back(dirty_id);
                    } else {
                        dirty.enqueue_reindex.push_back(dirty_id);
                    }
                }

                // The saved content is a compile input of every TU that
                // transitively includes it: open dependents recompile, closed
                // ones reindex so cross-file references stop serving the
                // pre-save state. Enqueueing is O(1) per TU and deliberately
                // uncapped — the index's content-hash staleness check filters
                // TUs whose dependencies did not actually change, and the
                // idle/priority scheduling throttles the rest.
                // TODO: observe on large projects before adding debouncing.
                auto split_dependents = [&](llvm::ArrayRef<std::uint32_t> roots) {
                    for(auto root: roots) {
                        if(store.find(root)) {
                            dirty.mark_ast_dirty.push_back(root);
                        } else {
                            dirty.enqueue_reindex.push_back(root);
                        }
                    }
                };
                split_dependents(old_dependents);
                split_dependents(workspace.dep_graph.find_host_sources(path_id));

                // Headers whose resolved context embeds the saved file
                // through its include chain must re-synthesize their
                // preamble: it copies the chain files' content, so neither
                // the dependents cascade above nor clang's own dependency
                // tracking catches this.
                for(auto header_id: contexts.chain_dependents(path_id)) {
                    dirty.force_revalidate.push_back(header_id);
                    // The chain change may have made the header
                    // self-contained (e.g. a dependency now provides the
                    // missing declarations); drop the persisted verdict so
                    // the trial can downgrade it.
                    dirty.reset_header_mode.push_back(header_id);
                    // Contexts outlive their sessions: a closed header's
                    // shard rows were indexed under the old chain and only a
                    // background reindex can refresh them.
                    if(!store.find(header_id)) {
                        dirty.enqueue_reindex.push_back(header_id);
                    }
                }

                // A save can remove the include edge a user's context choice
                // depends on; the include graph was already rescanned above.
                dirty.recheck_contexts = true;
                dirty.reschedule_indexing = true;
                break;
            }
            case FileEvent::Kind::BufferClosed: {
                // Disk is the truth again: update the module graph and hand
                // the file back to the background indexer, whose shard now
                // supersedes the dropped session's index.
                workspace.on_file_closed(event.path_id);
                dirty.enqueue_reindex.push_back(event.path_id);
                dirty.reschedule_indexing = true;
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
                // Context validation, persistence and session reset happen in
                // ContextResolver::switch_context, which already lives in the
                // right module; this case is a hook for future cross-file
                // policy.
                break;
            }
            case FileEvent::Kind::WorkerCrashed: {
                // The worker's ASTs are gone; every document it owned must
                // recompile. Compile inputs did not change, so trial state
                // and self-containment verdicts stay untouched.
                for(auto path_id: event.paths) {
                    dirty.mark_lost.push_back(path_id);
                }
                break;
            }
        }
    }

    dedup(dirty.mark_ast_dirty);
    dedup(dirty.mark_lost);
    dedup(dirty.reset_trial);
    dedup(dirty.reset_header_mode);
    dedup(dirty.force_revalidate);
    dedup(dirty.enqueue_reindex);
    return dirty;
}

}  // namespace clice

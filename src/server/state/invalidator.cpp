#include "server/state/invalidator.h"

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

void Invalidator::cascade_compile_graph(std::uint32_t path_id, DirtySet& dirty) {
    if(!workspace.compile_graph || !workspace.compile_graph->has_unit(path_id)) {
        return;
    }
    for(auto dirty_id: workspace.compile_graph->update(path_id)) {
        workspace.pcm_paths.erase(dirty_id);
        workspace.pcm_cache.erase(dirty_id);
        if(store.find(dirty_id)) {
            dirty.mark_ast_dirty.push_back(dirty_id);
        } else {
            dirty.enqueue_reindex.push_back(dirty_id);
        }
    }
}

void Invalidator::cascade_disk_content_change(std::uint32_t path_id, DirtySet& dirty) {
    // The file's own self-containment may have changed; re-evaluate on its
    // next compile.
    dirty.reset_header_mode.push_back(path_id);
    dirty.reset_trial.push_back(path_id);

    // Root TUs transitively including the file, snapshotted before the
    // rescan rewrites the include graph. A content change only rewrites
    // the file's own outgoing edges, so this set normally equals the
    // post-rescan one — the pre-rescan snapshot is a cheap safety net for
    // a reverse map that was stale when the change landed.
    auto old_dependents = workspace.dep_graph.find_host_sources(path_id);

    // Rescan disk state (include edges, module declaration, compile-graph
    // cascade, PCM caches); the cascade names the module units whose build
    // products went stale.
    auto dirtied = workspace.rescan_after_save(path_id);
    for(auto dirty_id: dirtied) {
        if(store.find(dirty_id)) {
            dirty.mark_ast_dirty.push_back(dirty_id);
        } else {
            dirty.enqueue_reindex.push_back(dirty_id);
        }
    }

    // The new content is a compile input of every TU that transitively
    // includes it: open dependents recompile, closed ones reindex so
    // cross-file references stop serving the stale state. Enqueueing is
    // O(1) per TU and deliberately uncapped — the index's content-hash
    // staleness check filters TUs whose dependencies did not actually
    // change, and the idle/priority scheduling throttles the rest.
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

    // Headers whose resolved context embeds the file through its include
    // chain must re-synthesize their preamble: it copies the chain files'
    // content, so neither the dependents cascade above nor clang's own
    // dependency tracking catches this.
    for(auto header_id: contexts.chain_dependents(path_id)) {
        dirty.force_revalidate.push_back(header_id);
        // The chain change may have made the header self-contained (e.g. a
        // dependency now provides the missing declarations); drop the
        // persisted verdict so the trial can downgrade it.
        dirty.reset_header_mode.push_back(header_id);
        // Contexts outlive their sessions: a closed header's shard rows
        // were indexed under the old chain and only a background reindex
        // can refresh them.
        if(!store.find(header_id)) {
            dirty.enqueue_reindex.push_back(header_id);
        }
    }

    // A content change can remove the include edge a user's context choice
    // depends on; the include graph was already rescanned above.
    dirty.recheck_contexts = true;
    dirty.reschedule_indexing = true;
}

DirtySet Invalidator::apply(llvm::ArrayRef<FileEvent> events) {
    DirtySet dirty;

    // DiskRemoved defers its reverse-map rebuild here so a batch of
    // removals pays for one rebuild, not one per file.
    bool rebuild_reverse_map = false;

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
                // The disk now holds the buffer's content: the standard
                // disk-content cascade covers everything a save invalidates.
                cascade_disk_content_change(path_id, dirty);

                // ... unless a save hook or formatter rewrote the file as it
                // landed, leaving the disk ahead of the buffer. Dependents
                // already read the rewritten disk through the cascade above;
                // without this check the saved file itself would keep serving
                // results whose deps snapshot describes a disk state that no
                // longer exists ("I see my old buffer, my dependents see the
                // new disk"). Recompiling does not change what the session
                // compiles — an open file's own text always comes from its
                // buffer — but it re-captures the deps snapshot and re-runs
                // preamble/PCH validation against the rewritten disk, which
                // the pull-side staleness check alone can miss when the
                // rewrite lands within mtime granularity of the compile.
                if(auto session = store.find(path_id)) {
                    auto disk = read_file(workspace.path_pool.resolve(path_id));
                    if(!disk || *disk != session->text) {
                        dirty.mark_ast_dirty.push_back(path_id);
                    }
                }
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
                auto path_id = event.path_id;
                if(store.find(path_id)) {
                    // Open file: the buffer is the truth, so no disk rescan —
                    // what the disk change means for this file is decided by
                    // the next compile's deps validation. Recompile so that
                    // validation actually runs.
                    dirty.mark_ast_dirty.push_back(path_id);
                    break;
                }
                // Closed file: disk is the truth. Run the same cascade a
                // save does, and refresh the file's own now-stale shard.
                cascade_disk_content_change(path_id, dirty);
                dirty.enqueue_reindex.push_back(path_id);
                break;
            }
            case FileEvent::Kind::DiskRemoved: {
                auto path_id = event.path_id;
                // Dependents compile against a now-missing include: open
                // ones recompile (the missing-file diagnostic is the truth),
                // closed ones reindex — nothing else would ever queue them.
                // Snapshot before the scrub below rewrites the graph.
                for(auto root: workspace.dep_graph.find_host_sources(path_id)) {
                    if(store.find(root)) {
                        dirty.mark_ast_dirty.push_back(root);
                    } else {
                        dirty.enqueue_reindex.push_back(root);
                    }
                }
                // A removed module unit takes its PCM with it: importers'
                // build products went stale, and it stops providing its
                // module name.
                cascade_compile_graph(path_id, dirty);
                workspace.path_to_module.erase(path_id);
                // Scrub the includer role: the file's outgoing edges vanished
                // with it, so it stops being a host-source candidate.
                // Incoming edges stay — includers' text still names it, and
                // their own rescan owns those edges. The reverse-map rebuild
                // is deferred to the end of the batch: a mass deletion (git
                // checkout) would otherwise rebuild it once per file, and
                // within-batch cascades tolerate a stale reverse map by
                // design (they union the pre/post snapshots).
                workspace.dep_graph.clear_includes(path_id);
                rebuild_reverse_map = true;
                workspace.context_epoch += 1;
                // Contexts hosted by (or chained through) the removed file
                // are cleaned by the resolver's orphan pass.
                dirty.recheck_contexts = true;
                dirty.reschedule_indexing = true;
                // Index shards are deliberately kept: the last-known content
                // still serves navigation.
                // TODO: sweep orphaned shards of files that stay deleted.
                break;
            }
            case FileEvent::Kind::CDBChanged: {
                auto& delta = event.cdb;
                if(delta.empty()) {
                    break;
                }

                // The producer already reloaded the CDB; derived state must
                // follow. Rebuild the include graph and module map from
                // scratch against the new database: entry additions,
                // removals and flag changes all funnel into one uniform
                // rescan instead of per-entry graph surgery. No ScanCache is
                // retained anywhere: the cache's contract requires clearing
                // it on every CDB change, and CDB changes are the only
                // rescan trigger, so a persistent cache would never be warm.
                // TODO: this scan runs synchronously on the event loop (same
                // cost as the startup scan); if it shows up on large
                // projects, move it off the dispatch path.
                workspace.dep_graph = DependencyGraph();
                scan_dependency_graph(workspace.cdb,
                                      workspace.toolchain,
                                      workspace.path_pool,
                                      workspace.dep_graph,
                                      /*cache=*/nullptr,
                                      [this](llvm::StringRef path,
                                             std::vector<std::string>& append,
                                             std::vector<std::string>& remove) {
                                          workspace.config.match_rules(path, append, remove);
                                      });
                workspace.dep_graph.build_reverse_map();
                workspace.path_to_module.clear();
                workspace.build_module_map();
                workspace.context_epoch += 1;

                // Every delta entry needs the same treatment — the compile
                // command is an input that content-based staleness cannot
                // see, whether it appeared, changed or vanished. PCH/PCM
                // keys embed the canonical flags, so pull-side caches miss
                // naturally.
                auto invalidate_entry = [&](std::uint32_t path_id, bool keep_shard) {
                    if(store.find(path_id)) {
                        // The next compile re-resolves the command (added:
                        // first real entry replaces the guessed one;
                        // changed: new flags; removed: fall back).
                        dirty.mark_ast_dirty.push_back(path_id);
                    } else if(!keep_shard) {
                        // The shard was indexed under the old command, and
                        // the indexer's freshness gate validates content
                        // only: evict the shard so the queued reindex is
                        // not filtered out as fresh.
                        // TODO: a background index task already in flight
                        // can merge its old-command result back after this
                        // eviction; closing that window needs an index
                        // generation guard in the indexer.
                        workspace.merged_indices.erase(path_id);
                        dirty.enqueue_reindex.push_back(path_id);
                    }

                    // A module unit's command change invalidates importers'
                    // PCMs (no-op for files the compile graph doesn't know).
                    cascade_compile_graph(path_id, dirty);

                    // The file's own resolved header context was built on a
                    // command that no longer exists in that form (a header
                    // gaining its first exact entry included), and so was
                    // every header context hosted by this file. Drop them
                    // so the next use re-resolves.
                    if(contexts.header_context(path_id)) {
                        dirty.drop_context.push_back(path_id);
                    }
                    for(auto& [header_id, context]: contexts.header_contexts) {
                        if(context.host_path_id != path_id) {
                            continue;
                        }
                        dirty.drop_context.push_back(header_id);
                        if(store.find(header_id)) {
                            dirty.mark_ast_dirty.push_back(header_id);
                        } else {
                            workspace.merged_indices.erase(header_id);
                            dirty.enqueue_reindex.push_back(header_id);
                        }
                    }
                };

                for(auto path_id: delta.added) {
                    invalidate_entry(path_id, /*keep_shard=*/false);
                }
                for(auto path_id: delta.changed) {
                    invalidate_entry(path_id, /*keep_shard=*/false);
                }
                for(auto path_id: delta.removed) {
                    // A removed entry keeps its shard: the last-known
                    // content still serves navigation, same conservative
                    // semantics as DiskRemoved. The graph rebuild above
                    // already dropped the file's source role, and the
                    // orphan recheck cleans choices through it.
                    invalidate_entry(path_id, /*keep_shard=*/true);
                }

                // The first CDB of the session may have introduced C++20
                // modules; the compile graph is otherwise created at startup.
                // TODO: a reload that adds a brand-new module unit to an
                // already existing graph is not registered (update() only
                // touches known units) — importers resolved before it
                // existed keep their stale dependency lists until restart.
                // Rebuilding the graph mid-session needs coordination with
                // in-flight compiles.
                if(!workspace.compile_graph) {
                    dirty.ensure_compile_graph = true;
                }

                dirty.recheck_contexts = true;
                dirty.reschedule_indexing = true;
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

    if(rebuild_reverse_map) {
        workspace.dep_graph.build_reverse_map();
    }

    dedup(dirty.mark_ast_dirty);
    dedup(dirty.mark_lost);
    dedup(dirty.reset_trial);
    dedup(dirty.reset_header_mode);
    dedup(dirty.force_revalidate);
    dedup(dirty.enqueue_reindex);
    dedup(dirty.drop_context);
    return dirty;
}

}  // namespace clice

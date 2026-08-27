#include "server/state/invalidator.h"

#include <utility>

#include "sched/families/pcm.h"
#include "sched/families/turun.h"
#include "server/service/ast_family.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringMap.h"
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
                         PCMFamily& pcm,
                         ReadFile read_file) :
    workspace(workspace), store(store), contexts(contexts), pcm(pcm),
    read_file(read_file ? std::move(read_file) : ReadFile(read_from_disk)) {}

/// Batch effects may name the same file twice (two saves in one batch);
/// execution must see each id once.
static void dedup(llvm::SmallVector<std::uint32_t>& ids) {
    llvm::sort(ids);
    ids.erase(llvm::unique(ids), ids.end());
}

/// An invalidated dependent recompiles when its session invests in an
/// AST, reindexes when closed — and does both for an index-only session
/// (freshness clause 4): the buffer is the compile truth, but the serving
/// rows are the shard's, and only a reindex refreshes those.
void Invalidator::mark_dependent(std::uint32_t path_id, DirtySet& dirty) {
    if(auto session = store.find(path_id)) {
        dirty.mark_ast_dirty.push_back(path_id);
        if(session->serving == ServingMode::IndexOnly) {
            dirty.add_reindex_deps_only(path_id);
        }
    } else {
        dirty.add_reindex_deps_only(path_id);
    }
}

void Invalidator::cascade_compile_graph(std::uint32_t path_id, DirtySet& dirty) {
    if(!pcm.tracks(path_id)) {
        return;
    }
    for(auto dirty_id: pcm.invalidate(path_id)) {
        mark_dependent(dirty_id, dirty);
    }
}

void Invalidator::provider_appeared(llvm::StringRef module_name, DirtySet& dirty) {
    // Every consumer whose scan met the name unresolved holds a durable
    // edge to its sentinel; the graph cascade is the complete list — no
    // side bookkeeping, no reverse-map walk. Their rows lack the
    // module's symbols and their dep snapshots never named the
    // interface, so the content-hash gate would filter a DepsOnly
    // reindex — ContentChanged bypasses it. Nothing is dropped: a
    // rebuild replaces the rows, and a unit that can no longer build
    // (retired entry, deleted file) keeps serving its last-known ones.
    for(auto id: pcm.provider_appeared(module_name)) {
        if(PCMFamily::is_unresolved(id)) {
            continue;
        }
        auto path_id = static_cast<std::uint32_t>(id.key);
        if(id.family == turun_family) {
            dirty.add_reindex_content_changed(path_id);
        } else if(id.family == ast_family) {
            if(auto session = store.find(path_id)) {
                dirty.mark_ast_dirty.push_back(path_id);
                if(session->serving == ServingMode::IndexOnly) {
                    dirty.add_reindex_content_changed(path_id);
                }
            }
        } else {
            // A dirtied module unit: the family already dropped its
            // cached PCM state; its importers are in this same list.
            mark_dependent(path_id, dirty);
        }
    }
}

void Invalidator::rescan_disk_state(std::uint32_t path_id, DirtySet& dirty) {
    auto old_module = workspace.path_to_module.lookup(path_id);
    workspace.rescan_after_save(path_id);
    auto it = workspace.path_to_module.find(path_id);
    llvm::StringRef new_module =
        it != workspace.path_to_module.end() ? it->second : llvm::StringRef();
    if(new_module == old_module) {
        return;
    }

    // A rescan that introduced a module declaration may have given the
    // name its first provider: consumers that scanned it unresolved hold
    // edges to its sentinel, not to any real node a module-graph cascade
    // could reach.
    if(!new_module.empty() && workspace.dep_graph.lookup_module(new_module).size() == 1) {
        provider_appeared(new_module, dirty);
    }

    // The dropped name's consumers hold edges to this provider's real
    // node, and their builds embed a module the file no longer declares.
    // The close path has no other probe for this: an evicted PCM leaves
    // no cache entry for its staleness check to see.
    if(!old_module.empty()) {
        cascade_compile_graph(path_id, dirty);
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

    // Rescan disk state (include edges, module declaration); then cascade
    // through the module graph — importers' build products went stale, and
    // the cascade names every affected module unit.
    rescan_disk_state(path_id, dirty);
    cascade_compile_graph(path_id, dirty);

    // The new content is a compile input of every TU that transitively
    // includes it: open dependents recompile, closed ones reindex so
    // cross-file references stop serving the stale state. Enqueueing is
    // O(1) per TU and deliberately uncapped — the index's content-hash
    // staleness check filters TUs whose dependencies did not actually
    // change, and the idle/priority scheduling throttles the rest.
    // TODO: observe on large projects before adding debouncing.
    auto split_dependents = [&](llvm::ArrayRef<std::uint32_t> roots) {
        for(auto root: roots) {
            mark_dependent(root, dirty);
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
        // can refresh them. The header's own content did not change, so
        // its rows keep serving meanwhile. An open index-only session is
        // in the same boat — its shard is what the LSP serves.
        auto session = store.find(header_id);
        if(!session || session->serving == ServingMode::IndexOnly) {
            dirty.add_reindex_deps_only(header_id);
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
                // disk-content cascade covers everything a save invalidates —
                // including anything a DiskChanged consumed while the buffer
                // was open still owed, so that debt is discharged here.
                cascade_disk_content_change(path_id, dirty);
                disk_changed_while_open.erase(path_id);

                // The file's own shard describes the pre-save disk. With
                // open-file indexing off the queued slot is skipped and
                // BufferClosed repairs on close; with it on (an agent is
                // around), the reindex lands promptly. Saves only come from
                // open buffers — the session check just drops synthetic
                // events for files nobody has open.
                if(store.find(path_id)) {
                    dirty.add_reindex_content_changed(path_id);
                }

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
                workspace.on_file_closed(event.path_id);
                // Drained on every close — the deleted-while-open exit below
                // (whose debt passes to DiskRemoved semantics) must not
                // leave a stale entry behind.
                bool changed_while_open = disk_changed_while_open.erase(event.path_id);
                // Whether the shard's rows still describe the disk decides
                // how queries treat the file until the reindex lands: a
                // browse-and-close must not blank the file's references for
                // the queue's latency, while a close after saved edits must
                // not serve rows for text that no longer exists. One disk
                // read settles it; an unreadable file counts as changed.
                auto disk = read_file(workspace.path_pool.resolve(event.path_id));
                if(!disk) {
                    // Deleted while it was open: the tracker skips open
                    // files, so this close is the first observation of the
                    // missing file. Keep any shard serving (same deliberate
                    // choice as DiskRemoved) instead of recording a
                    // ContentChanged that would suppress it forever; the
                    // tracker's next sweep observes the removal and delivers
                    // the full DiskRemoved cascade.
                    dirty.add_clear_reindex(event.path_id);
                    break;
                }
                auto shard_it = workspace.shards.find(event.path_id);
                bool has_shard = shard_it != workspace.shards.end();
                bool shard_current = has_shard && shard_it->second.matches_content(*disk);
                // A module unit's PCM can be staler than the shard: an
                // agent-mode reindex reads the rewritten disk while the
                // artifact keeps the pre-change bytes. Its own deps
                // snapshot is the judge; checked before the cascade below
                // erases the entry.
                auto pcm_it = workspace.pcm_cache.find(event.path_id);
                bool pcm_stale = pcm_it != workspace.pcm_cache.end() &&
                                 deps_changed(workspace.path_pool, pcm_it->second.deps);
                // Disk is the truth again, and this close is the last
                // chance to act on it: the DiskChanged path deliberately
                // skips the rescan and the module/dependent cascades while
                // a buffer is open, and the tracker has already consumed
                // the event's mtime, so no later sweep will refire it.
                // Divergence — rows or artifact built from bytes the disk
                // no longer holds, or a disk change recorded while the
                // buffer was open (an agent-mode reindex can refresh the
                // shard from the rewritten disk before the close, blinding
                // the content probe while dependents still embed the old
                // bytes) — gets the full disk-content cascade a save would
                // have delivered. A file with no shard and no recorded
                // change is no evidence either way: indexing simply never
                // reached it, and cascading would tax every close.
                if((has_shard && !shard_current) || pcm_stale || changed_while_open) {
                    cascade_disk_content_change(event.path_id, dirty);
                } else if(has_shard) {
                    // The shard can be current while the edges are not:
                    // an agent-mode reindex refreshed the rows from the
                    // rewritten disk while the include graph kept the
                    // pre-change edges (open files skip the rescan).
                    // Refresh the edges alone — the rows are proven
                    // current, so no content cascade; a module name the
                    // rewrite introduced still reaches its sentinel-edged
                    // consumers through the rescan.
                    rescan_disk_state(event.path_id, dirty);
                }
                if(shard_current) {
                    dirty.add_reindex_deps_only(event.path_id);
                } else {
                    dirty.add_reindex_content_changed(event.path_id);
                }
                dirty.reschedule_indexing = true;
                break;
            }
            case FileEvent::Kind::DiskChanged: {
                auto path_id = event.path_id;
                if(store.find(path_id)) {
                    // Open file: the buffer is the truth, so no disk rescan —
                    // what the disk change means for this file is decided by
                    // the next compile's deps validation. Recompile so that
                    // validation actually runs. The shard describes the old
                    // disk regardless of the buffer; queue its reindex like
                    // a save (skipped-and-repaired-on-close without agents).
                    // The dependent cascade is deferred to the close, and
                    // the tracker has consumed the event — record the debt,
                    // or an agent-mode reindex that freshens the shard
                    // before the close would hide it from the close-time
                    // divergence probe.
                    dirty.mark_ast_dirty.push_back(path_id);
                    dirty.add_reindex_content_changed(path_id);
                    disk_changed_while_open.insert(path_id);
                    break;
                }
                // Closed file: disk is the truth. Run the same cascade a
                // save does, and refresh the file's own now-stale shard.
                cascade_disk_content_change(path_id, dirty);
                dirty.add_reindex_content_changed(path_id);
                break;
            }
            case FileEvent::Kind::DiskRemoved: {
                auto path_id = event.path_id;
                // Dependents compile against a now-missing include: open
                // ones recompile (the missing-file diagnostic is the truth),
                // closed ones reindex — nothing else would ever queue them.
                // Snapshot before the scrub below rewrites the graph.
                for(auto root: workspace.dep_graph.find_host_sources(path_id)) {
                    mark_dependent(root, dirty);
                }
                // A removed module unit takes its PCM with it: importers'
                // build products went stale, and it stops providing its
                // module name.
                cascade_compile_graph(path_id, dirty);
                // The file's shard deliberately keeps serving navigation
                // (its content snapshot is the only remaining truth), so any
                // pending reindex reason recorded before the removal — e.g.
                // a DiskChanged observed moments earlier — must be dropped:
                // there is nothing to reindex any more, and a lingering
                // ContentChanged would suppress the shard forever. Emitted
                // after the compile-graph cascade, which lists the removed
                // module itself among its dirtied units: the removal is this
                // event's final word for the file itself.
                dirty.add_clear_reindex(path_id);
                workspace.path_to_module.erase(path_id);
                // The provider leaves the module map too, or a later
                // replacement provider would sit behind the deleted one in
                // the candidate list and never be selected.
                workspace.dep_graph.update_module_decl(path_id, {});
                // The file's import syntax is gone with it: deleting the
                // last import-bearing file must release the project-wide
                // scan gate.
                workspace.dep_graph.set_import_candidate(path_id, false);
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
                // Per name, the provider import resolution selects
                // (direct_deps takes the list head) — not mere existence:
                // a reload can move the selection to another provider
                // while the old one's own entry stays unchanged.
                llvm::StringMap<std::uint32_t> selected_provider;
                for(auto& entry: workspace.dep_graph.modules()) {
                    if(!entry.getValue().empty()) {
                        selected_provider[entry.getKey()] = entry.getValue().front();
                    }
                }

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

                // A module name that just gained its first provider: its
                // sentinel's dependents are the TUs that scanned it
                // unresolved — the delta walk below cannot reach them
                // (they hold no edge to any real node). A name whose
                // selection moved to another provider: importers hold
                // edges to the old selected node, and when its own entry
                // is unchanged the delta walk cannot reach them either —
                // cascade from that node so their next rounds re-resolve.
                for(auto& entry: workspace.dep_graph.modules()) {
                    if(entry.getValue().empty()) {
                        continue;
                    }
                    auto it = selected_provider.find(entry.getKey());
                    if(it == selected_provider.end()) {
                        provider_appeared(entry.getKey(), dirty);
                    } else if(it->second != entry.getValue().front()) {
                        cascade_compile_graph(it->second, dirty);
                    }
                }

                // Every delta entry needs the same treatment — the compile
                // command is an input that content-based staleness cannot
                // see, whether it appeared, changed or vanished. PCH/PCM
                // keys embed the canonical flags, so pull-side caches miss
                // naturally.
                auto invalidate_entry = [&](std::uint32_t path_id, bool keep_index) {
                    if(store.find(path_id)) {
                        // The next compile re-resolves the command (added:
                        // first real entry replaces the guessed one;
                        // changed: new flags; removed: fall back).
                        dirty.mark_ast_dirty.push_back(path_id);
                    }
                    if(!keep_index) {
                        // The index was built under the old command, and
                        // the indexer's freshness gate validates content
                        // only: drop the TU's index so the queued reindex
                        // is not filtered out as fresh — in this session
                        // or after a restart. ContentChanged: a new
                        // command can rewrite the rows (macros, includes)
                        // as thoroughly as an edit.
                        dirty.drop_index.push_back(path_id);
                        dirty.add_reindex_content_changed(path_id);
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
                        // A standalone-indexed header borrowed the changed
                        // command too; its manifest is as stale as the
                        // host's (no-op for headers indexed only via TUs).
                        dirty.drop_index.push_back(header_id);
                        if(auto session = store.find(header_id)) {
                            dirty.mark_ast_dirty.push_back(header_id);
                            // An index-only session just lost its serving
                            // rows with the drop; only a reindex under the
                            // new command brings them back.
                            if(session->serving == ServingMode::IndexOnly) {
                                dirty.add_reindex_content_changed(header_id);
                            }
                        } else {
                            dirty.add_reindex_content_changed(header_id);
                        }
                    }
                };

                for(auto path_id: delta.added) {
                    invalidate_entry(path_id, /*keep_index=*/false);
                }
                for(auto path_id: delta.changed) {
                    invalidate_entry(path_id, /*keep_index=*/false);
                }
                for(auto path_id: delta.removed) {
                    // A removed entry keeps its index: the last-known
                    // content still serves navigation, same conservative
                    // semantics as DiskRemoved. The graph rebuild above
                    // already dropped the file's source role, and the
                    // orphan recheck cleans choices through it. Import
                    // bookkeeping stays: the file may live on as an
                    // included header (the rebuild re-marked it), and a
                    // truly retired entry is fenced by the reindexable
                    // gate in dirty_unresolved_importer.
                    invalidate_entry(path_id, /*keep_index=*/true);
                }

                dirty.recheck_contexts = true;
                dirty.reschedule_indexing = true;
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
            case FileEvent::Kind::DocumentEvicted: {
                // Same loss as a crash, scoped to one document: without the
                // recompile, feature requests re-route to a worker that no
                // longer holds the AST and silently return null.
                dirty.mark_lost.push_back(event.path_id);
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
    dedup(dirty.reindex_content_changed);
    dedup(dirty.reindex_deps_only);
    dedup(dirty.drop_index);
    dedup(dirty.drop_context);
    return dirty;
}

}  // namespace clice

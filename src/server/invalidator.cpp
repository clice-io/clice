#include "server/invalidator.h"

#include <utility>

#include "sched/families/pcm.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"

namespace clice {

Invalidator::Invalidator(Project& project,
                         const SessionStore& store,
                         const EditorContext& contexts,
                         PCMFamily& pcm,
                         const IndexStore& index) :
    project(project), store(store), contexts(contexts), pcm(pcm), index(index) {}

llvm::SmallVector<FileEvent> take_disk_events(FileTable& files) {
    llvm::SmallVector<FileEvent> events;
    for(auto path_id: files.take_changes()) {
        events.push_back(files.seen_missing(path_id) ? FileEvent::disk_removed(path_id)
                                                     : FileEvent::disk_changed(path_id));
    }
    return events;
}

/// Batch effects may name the same file twice (two saves in one batch);
/// execution must see each id once.
static void dedup(llvm::SmallVector<Fid>& ids) {
    llvm::sort(ids);
    ids.erase(llvm::unique(ids), ids.end());
}

/// An invalidated dependent recompiles when its session invests in an
/// AST, reindexes when closed — and does both for an index-only session
/// (freshness clause 4): the buffer is the compile truth, but the serving
/// rows are the shard's, and only a reindex refreshes those.
void Invalidator::mark_dependent(Fid path_id, DirtySet& dirty) {
    if(auto session = store.find(path_id)) {
        dirty.mark_ast_dirty.push_back(path_id);
        if(session->serving == ServingMode::IndexOnly) {
            dirty.add_reindex_deps_only(path_id);
        }
    } else {
        dirty.add_reindex_deps_only(path_id);
    }
}

void Invalidator::cascade_compile_graph(Fid path_id, DirtySet& dirty) {
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
        auto path_id = Fid{static_cast<std::uint32_t>(id.key)};
        if(id.family == Family::TURun) {
            dirty.add_reindex_content_changed(path_id);
        } else if(id.family == Family::AST) {
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

void Invalidator::rescan_disk_state(Fid path_id, DirtySet& dirty) {
    std::string old_module(project.dep_graph.module_of(path_id));
    project.rescan_after_save(path_id);
    auto new_module = project.dep_graph.module_of(path_id);
    if(new_module == old_module) {
        return;
    }

    // A rescan that introduced a module declaration may have given the
    // name its first provider: consumers that scanned it unresolved hold
    // edges to its sentinel, not to any real node a module-graph cascade
    // could reach.
    if(!new_module.empty() && project.dep_graph.lookup_module(new_module).size() == 1) {
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

void Invalidator::cascade_disk_content_change(Fid path_id, DirtySet& dirty) {
    // The file's own self-containment may have changed; re-evaluate on its
    // next compile.
    dirty.reset_header_mode.push_back(path_id);
    dirty.reset_trial.push_back(path_id);

    // Root TUs transitively including the file: the ones the lexical scan
    // sees, and the ones whose indexed compile read it — the scan cannot
    // resolve a macro include. The rescan below rewrites only the file's
    // own outgoing edges, never the includers this walks.
    auto dependents = project.dep_graph.find_host_sources(path_id);
    if(auto it = project.project_index.contributions.find(path_id);
       it != project.project_index.contributions.end()) {
        for(auto& [tu, rows]: it->second) {
            if(tu != path_id && !llvm::is_contained(dependents, tu)) {
                dependents.push_back(tu);
            }
        }
    }

    // Rescan disk state (include edges, module declaration); then cascade
    // through the module graph — importers' build products went stale, and
    // the cascade names every affected module unit.
    rescan_disk_state(path_id, dirty);
    cascade_compile_graph(path_id, dirty);
    // Module units whose PCM read the file (a header in a global module
    // fragment): the artifact's own record names them, the module graph
    // knows only imports.
    llvm::SmallVector<Fid> readers;
    for(auto& [unit, state]: project.pcm_cache) {
        if(unit != path_id &&
           llvm::any_of(state.deps, [&](const DepState& dep) { return dep.path_id == path_id; })) {
            readers.push_back(unit);
        }
    }
    for(auto unit: readers) {
        cascade_compile_graph(unit, dirty);
    }

    // The new content is a compile input of every TU that transitively
    // includes it: open dependents recompile, closed ones reindex so
    // cross-file references stop serving the stale state. Enqueueing is
    // O(1) per TU and deliberately uncapped — the index's content-hash
    // staleness check filters TUs whose dependencies did not actually
    // change, and the idle/priority scheduling throttles the rest.
    // TODO: observe on large projects before adding debouncing.
    for(auto root: dependents) {
        mark_dependent(root, dirty);
    }

    // Headers whose resolved context was derived from the file through its
    // include chain resolve it again: the synthesized preamble copies the
    // chain files' content, so neither the dependents cascade above nor
    // clang's own dependency tracking catches this.
    for(auto header_id: contexts.chain_dependents(path_id)) {
        dirty.drop_context.push_back(header_id);
        // The chain change may have made the header self-contained (e.g. a
        // dependency now provides the missing declarations); drop the
        // persisted verdict so the trial can downgrade it.
        dirty.reset_header_mode.push_back(header_id);
        auto session = store.find(header_id);
        if(session) {
            dirty.mark_ast_dirty.push_back(header_id);
        }
        // Contexts outlive their sessions: a closed header's shard rows
        // were indexed under the old chain and only a background reindex
        // can refresh them. The header's own content did not change, so
        // its rows keep serving meanwhile. An open index-only session is
        // in the same boat — its shard is what the LSP serves.
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

    // The lender set changed: every borrowed or synthesized command may
    // resolve differently now — which no delta can tell, so all of them
    // recompile.
    auto lenders_changed = [&] {
        project.commands_epoch += 1;
        for(auto guessed: contexts.guessed_commands) {
            if(store.find(guessed)) {
                dirty.mark_ast_dirty.push_back(guessed);
            }
        }
    };
    for(auto& event: events) {
        switch(event.kind) {
            case FileEvent::Kind::DiskChanged: {
                // Whether or not a buffer is open: the disk is what every
                // other file compiles against and what the index describes.
                cascade_disk_content_change(event.path_id, dirty);
                dirty.add_reindex_content_changed(event.path_id);
                break;
            }
            case FileEvent::Kind::DiskRemoved: {
                auto path_id = event.path_id;
                // Dependents compile against a now-missing include: open
                // ones recompile (the missing-file diagnostic is the truth),
                // closed ones reindex — nothing else would ever queue them.
                // Snapshot before the scrub below rewrites the graph.
                for(auto root: project.dep_graph.find_host_sources(path_id)) {
                    mark_dependent(root, dirty);
                }
                // A removed module unit takes its PCM with it: importers'
                // build products went stale.
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
                project.forget_file(path_id);
                // Contexts hosted by (or chained through) the removed file
                // are cleaned by ContextService::drop_orphaned_choices.
                dirty.recheck_contexts = true;
                dirty.reschedule_indexing = true;
                // Index shards are deliberately kept: the last-known content
                // still serves navigation.
                // TODO: sweep orphaned shards of files that stay deleted.
                break;
            }
            case FileEvent::Kind::CDBChanged: {
                lenders_changed();
                auto& delta = event.cdb;
                if(delta.empty()) {
                    break;
                }

                // The producer already reloaded the CDB; derived state must
                // follow.
                auto providers = project.rebuild_dependency_graph();

                // A module name that just gained its first provider: its
                // sentinel's dependents are the TUs that scanned it
                // unresolved — the delta walk below cannot reach them
                // (they hold no edge to any real node). A name whose
                // selection moved to another provider: importers hold
                // edges to the old selected node, and when its own entry
                // is unchanged the delta walk cannot reach them either —
                // cascade from that node so their next rounds re-resolve.
                for(auto& name: providers.appeared) {
                    provider_appeared(name, dirty);
                }
                for(auto replaced: providers.replaced) {
                    cascade_compile_graph(replaced, dirty);
                }

                // Every delta entry needs the same treatment — the compile
                // command is an input that content-based staleness cannot
                // see, whether it appeared, changed or vanished. PCH/PCM
                // keys embed the canonical flags, so pull-side caches miss
                // naturally.
                auto invalidate_entry = [&](Fid path_id, bool retired) {
                    if(store.find(path_id)) {
                        // The next compile re-resolves the command (added:
                        // first real entry replaces the guessed one;
                        // changed: new flags; removed: fall back).
                        dirty.mark_ast_dirty.push_back(path_id);
                    }
                    if(retired) {
                        dirty.add_retire(path_id);
                    } else {
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
                    // A standalone-indexed header borrowed the changed
                    // command too, open or not: its manifest is as stale as
                    // the host's (no-op for headers indexed only via TUs).
                    auto borrowers = index.headers_hosted_by(path_id);
                    for(auto& [header_id, context]: contexts.header_contexts) {
                        if(context.host_path_id != path_id) {
                            continue;
                        }
                        dirty.drop_context.push_back(header_id);
                        if(store.find(header_id)) {
                            dirty.mark_ast_dirty.push_back(header_id);
                        }
                        if(!llvm::is_contained(borrowers, header_id)) {
                            borrowers.push_back(header_id);
                        }
                    }
                    for(auto header_id: borrowers) {
                        dirty.drop_index.push_back(header_id);
                        // An index-only session just lost its serving rows
                        // with the drop; only a reindex under the new
                        // command brings them back. An open session that
                        // compiles is reindexed when it closes.
                        auto session = store.find(header_id);
                        if(!session || session->serving == ServingMode::IndexOnly) {
                            dirty.add_reindex_content_changed(header_id);
                        }
                    }
                };

                for(auto path_id: delta.added) {
                    invalidate_entry(path_id, /*retired=*/false);
                }
                for(auto path_id: delta.changed) {
                    invalidate_entry(path_id, /*retired=*/false);
                }
                for(auto path_id: delta.removed) {
                    // The database that owned the entry reloaded fine and
                    // no longer lists the file: the build stopped
                    // compiling it, so its rows leave the index — unless a
                    // rule's default command still claims it, which makes
                    // this a command change. (A database that vanishes
                    // keeps serving its entries — the tracker never
                    // reloads a missing file — so this is not the
                    // DiskRemoved case.) The graph rebuild above already
                    // dropped a retired file's source role, and the
                    // orphan recheck cleans choices through it.
                    invalidate_entry(path_id,
                                     /*retired=*/project.build.commands(path_id).empty());
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

    dedup(dirty.mark_ast_dirty);
    dedup(dirty.mark_lost);
    dedup(dirty.reset_trial);
    dedup(dirty.reset_header_mode);
    dedup(dirty.reindex_content_changed);
    dedup(dirty.reindex_deps_only);
    dedup(dirty.drop_index);
    dedup(dirty.drop_context);
    return dirty;
}

}  // namespace clice

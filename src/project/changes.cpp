module clice;

import :command.database;
import :index.project_index;
import :project.background;
import :project.graph.pcm;
import :project.graph.task_graph;
import :project.project;
import :support.logging;
import :support.text;
import :syntax.dependency_graph;
import :vfs.file_table;

namespace clice {

namespace {

/// One change's walk through what derives from the files it changed: the
/// PCMs built from them, the units and the open documents reading them.
struct Cascade {
    Project& project;

    llvm::ArrayRef<OpenFile> open;

    Affected affected = {};

    bool opened(Fid file) const {
        return ranges::contains(open, file, &OpenFile::file);
    }

    /// An open document's compile read what changed: it compiles again.
    void mark_stale(Fid file) {
        if(opened(file) && !ranges::contains(affected.stale, file)) {
            affected.stale.push_back(file);
        }
    }

    /// Queue the file's background index again: `content` when its own
    /// content changed, else only a dependency's.
    void reindex(Fid file, bool content) {
        auto& background = project.background_index;
        background.forget_skipped_diagnostics(file);
        auto reason = content ? ReindexReason::ContentChanged : ReindexReason::DepsOnly;
        if(background.enqueue(file, reason) && opened(file) &&
           !ranges::contains(affected.reindexed, file)) {
            affected.reindexed.push_back(file);
        }
    }

    /// A dependency of the file changed: it recompiles when it is open, and
    /// reindexes.
    void mark_dependent(Fid file) {
        project.index.mark_outdated(file);
        mark_stale(file);
        reindex(file, false);
    }

    /// Every unit whose compile or build depends on the file — the single
    /// answer to "who read this": Project::readers, and the open documents
    /// whose last compile read it — the scan cannot resolve a macro
    /// include, never follows an import, and never sees a file before it
    /// exists.
    llvm::SmallVector<Fid> readers(Fid file) const {
        auto result = project.readers(file);
        for(auto& document: open) {
            if(document.file != file && ranges::contains(document.read, file) &&
               !ranges::contains(result, document.file)) {
                result.push_back(document.file);
            }
        }
        return result;
    }

    /// Treat every reader of a file that changed or vanished: open ones
    /// recompile, closed ones reindex, module units check their PCM again
    /// (which voids a build in flight).
    void mark_readers(llvm::ArrayRef<Fid> readers) {
        for(auto reader: readers) {
            invalidate_module(reader);
            mark_dependent(reader);
        }
    }

    /// Mark a module unit's PCMs and their transitive importers dirty, overtaking
    /// in-flight attempts: their next attempts check their builds again, and
    /// a build whose key and inputs still hold is kept — the single write
    /// point for PCM content invalidation. A product-only loss (cache
    /// eviction) is the node's check instead (Project::module_work), which
    /// re-dirties the module alone: what its importers read is unchanged.
    void invalidate_module(Fid file) {
        for(auto module: project.module_nodes(file)) {
            for(auto dirtied: project.graph.invalidate(module)) {
                mark_dependent(dirtied.file());
            }
        }
    }

    /// A module name just gained its first provider: invalidate the
    /// dependents of its unresolved import — their keys name the import as
    /// unresolved, so their next attempts rebuild — and route them to
    /// recompiles and ContentChanged reindexes.
    void provider_appeared(llvm::StringRef name) {
        // Every consumer whose scan met the name unresolved declared an
        // edge to its unresolved-import node; the graph cascade is the complete list — no
        // side bookkeeping, no reverse-map walk. Their shards lack the
        // module's symbols and their dep snapshots never named the
        // interface, so the content-hash gate would filter a DepsOnly
        // reindex — ContentChanged bypasses it. Nothing is dropped: a
        // rebuild replaces the shards, and a unit that can no longer build keeps
        // its last-known one (queries withhold a deleted file's).
        for(auto id: project.graph.invalidate(unresolved_import(name))) {
            if(is_unresolved_import(id)) {
                continue;
            }
            auto file = id.file();
            if(id.kind == NodeKind::Unit) {
                reindex(file, true);
            } else if(id.kind == NodeKind::AST) {
                if(opened(file)) {
                    mark_stale(file);
                    reindex(file, true);
                }
            } else {
                // A dirtied module unit: its importers are in this same list.
                mark_dependent(file);
            }
        }
    }

    /// The providers of `name` but `except` changed: the importers of every
    /// other one choose among them again.
    void cascade_providers(llvm::StringRef name, Fid except) {
        for(auto provider:
            ranges::to<llvm::SmallVector<Fid>>(project.dep_graph.lookup_module(name))) {
            if(provider != except) {
                invalidate_module(provider);
            }
        }
    }

    /// Rescan the file's disk state (include edges, module declaration), so
    /// host lookups and context queries see includes the change added or
    /// removed. A module name the rescan gave its first provider cascades to
    /// the consumers holding unresolved-import edges against it; a name the
    /// file stopped providing cascades through the provider's real node
    /// instead.
    void rescan(Fid file) {
        auto& graph = project.dep_graph;
        std::string old_module(graph.module_of(file));
        rescan_dependency_graph(project.cdb, project.toolchain, graph, file);
        auto new_module = graph.module_of(file);
        if(new_module == old_module) {
            return;
        }

        // A rescan that introduced a module declaration may have given the
        // name its first provider: consumers that scanned it unresolved hold
        // edges to its unresolved-import node, not to any real node a
        // module-graph cascade could reach.
        if(!new_module.empty()) {
            if(graph.lookup_module(new_module).size() == 1) {
                provider_appeared(new_module);
            }
            cascade_providers(new_module, file);
        }

        // The dropped name's consumers hold edges to this provider's real
        // node, and their builds embed a module the file no longer declares.
        // Nothing else on the close path finds this: an evicted PCM leaves
        // no cache entry for its staleness check to see.
        if(!old_module.empty()) {
            invalidate_module(file);
            cascade_providers(old_module, file);
        }
    }

    /// The file's on-disk content is new: rescan its disk state, then split
    /// every affected file into open (recompile) and closed (reindex).
    void content_changed(Fid file) {
        // Taken before the rescan below, which rewrites only the file's own
        // outgoing edges, never the includers this walks. A file the scan did
        // not know is new: its readers looked for it and failed, and their
        // rescans give the lexical graph the edges it lacked.
        auto dependents = readers(file);
        if(!project.dep_graph.knows(file)) {
            for(auto reader: dependents) {
                rescan(reader);
            }
        }

        // Rescan disk state (include edges, module declaration); then cascade
        // through the module graph — importers' build products went stale, and
        // the cascade names every affected module unit.
        rescan(file);
        invalidate_module(file);

        // The new content is an input of every unit that read it: open ones
        // recompile, closed ones reindex so cross-file references stop serving
        // the stale state, module units check their PCM again. Enqueueing is
        // O(1) per TU and deliberately uncapped — the index's content-hash
        // staleness check filters TUs whose dependencies did not actually
        // change, and the idle/priority scheduling throttles the rest.
        mark_readers(dependents);
    }

    /// The file vanished from disk.
    void removed(Fid file) {
        auto& graph = project.dep_graph;
        // Dependents compile against a now-missing include: open ones
        // recompile (the missing-file diagnostic is the truth), closed ones
        // reindex — nothing else would ever queue them. Taken before the
        // graph forgets the file below.
        mark_readers(readers(file));
        // A removed module unit takes its PCM with it: importers' build
        // products went stale.
        invalidate_module(file);
        cascade_providers(std::string(graph.module_of(file)), file);
        // Any reindex reason recorded before the removal — e.g. a change
        // observed moments earlier — is dropped: there is nothing to
        // reindex any more, and the file's shard stays behind (queries
        // withhold the shard of a file seen missing). After the
        // compile-graph cascade, which lists the removed module itself
        // among its dirtied units: the removal is the final word for the
        // file itself.
        project.background_index.forget(file);
        auto& reindexed = affected.reindexed;
        reindexed.erase(ranges::remove(reindexed, file).begin(), reindexed.end());
        // It stops providing its module name (a replacement provider would
        // otherwise sit behind it and never be selected) and its import
        // syntax (the last import-bearing file must release the
        // project-wide scan gate), and its outgoing edges go, so it stops
        // being a host candidate. Incoming edges stay — includers' text
        // still names it, and their own rescans own those edges.
        graph.update_module_decl(file, {});
        graph.forget_scan(file);
        graph.clear_includes(file);
        // TODO: remove orphaned shards of files that stay deleted.
    }
};

}  // namespace

llvm::SmallVector<Fid> Project::readers(Fid file) const {
    auto found = dep_graph.find_readers(file);
    llvm::SmallVector<Fid> result(found.begin(), found.end());
    auto add = [&](Fid reader) {
        if(reader != file && !ranges::contains(result, reader)) {
            result.push_back(reader);
        }
    };
    index.for_each_contribution(file, [&](Fid unit, std::uint64_t) { add(unit); });
    // LIMITATION: with indexing off there are no absent lookups: a header
    // created after the scan of a closed host whose `#include` missed it (a
    // generated `.inc`) has no host, and compiles alone with false errors,
    // until the host changes.
    index.for_each_absent_lookup(file, add);
    index.for_each_importer(file, add);
    for(auto& [module, entry]: pcms) {
        if(ranges::contains(entry.inputs, file, &Input::file) ||
           (entry.failure && ranges::contains(entry.failure->inputs, file, &Input::file))) {
            add(module.file());
        }
    }
    return result;
}

bool Project::knows(Fid file, llvm::ArrayRef<OpenFile> open) {
    return ranges::contains(open, file, &OpenFile::file) || !candidates(file).empty() ||
           dep_graph.knows(file) || !Cascade{*this, open}.readers(file).empty();
}

Affected Project::files_changed(llvm::ArrayRef<Fid> files, llvm::ArrayRef<OpenFile> open) {
    Cascade cascade{*this, open};
    for(auto file: files) {
        if(file_table.disk.seen_missing(file)) {
            cascade.removed(file);
            continue;
        }
        // Whether or not a buffer is open: the disk is what every other
        // file compiles against and what the index describes.
        cascade.content_changed(file);
        // A unit of its own reads the file as well.
        index.mark_outdated(file);
        cascade.reindex(file, true);
    }
    return std::move(cascade.affected);
}

Project::Rebuilt Project::rebuild_dependency_graph() {
    llvm::StringMap<llvm::SmallVector<Fid, 2>> providers;
    for(auto& entry: dep_graph.modules()) {
        if(!entry.getValue().empty()) {
            providers[entry.getKey()] = entry.getValue();
        }
    }

    // TODO: every CDB change rescans the whole graph synchronously on the
    // event loop, twice when provisional members change; make it
    // incremental or move it off the dispatch path.
    auto scan = [&] {
        dep_graph.reset();
        auto report = scan_dependency_graph(cdb, toolchain, dep_graph, scan_commands(members()));
        dep_graph.build_reverse_map();
        return report;
    };
    auto report = scan();
    Rebuilt changes{.provisional = refresh_provisional()};
    if(!changes.provisional.empty()) {
        report = scan();
    }

    std::size_t modules = 0;
    for(auto& entry: dep_graph.modules()) {
        if(entry.getValue().empty()) {
            continue;
        }
        modules += 1;
        auto it = providers.find(entry.getKey());
        if(it == providers.end()) {
            changes.appeared.push_back(entry.getKey().str());
            continue;
        }
        auto now = ranges::to<llvm::SmallVector<Fid>>(entry.getValue());
        ranges::sort(it->second);
        ranges::sort(now);
        if(it->second != now) {
            changes.replaced.append(it->second.begin(), it->second.end());
        }
    }

    auto found = report.includes_resolved + report.unresolved.size();
    auto includes = found == 0 ? std::string("no includes")
                               : std::format("{} of {} found ({:.1f}%)",
                                             report.includes_resolved,
                                             counted(found, "include", "includes"),
                                             100.0 * report.includes_resolved / found);
    LOG_INFO("Scanned {} in {} ms ({}, {}, {} unchanged): {}, {}",
             counted(report.total_files, "file", "files"),
             report.elapsed_ms,
             counted(report.source_files, "source", "sources"),
             counted(report.total_files - report.source_files, "header", "headers"),
             report.scan_cache_hits,
             includes,
             counted(modules, "module", "modules"));
    changes.scan = std::move(report);
    return changes;
}

std::optional<Affected> Project::ask_compilers_again(llvm::ArrayRef<OpenFile> open) {
    if(!toolchain.retry_failed()) {
        return std::nullopt;
    }
    Cascade cascade{.project = *this, .open = open};
    llvm::SmallVector<Fid> units;
    index.for_each_unit([&](Fid unit) { units.push_back(unit); });
    for(auto unit: units) {
        if(index.indexed_command(unit) != command_identity(unit)) {
            cascade.invalidate_module(unit);
            index.mark_outdated(unit);
            cascade.reindex(unit, true);
        }
    }
    background_index.schedule();
    return std::move(cascade.affected);
}

Affected Project::update_members(CDBDiff commands, llvm::ArrayRef<OpenFile> open) {
    if(commands.empty()) {
        // With nothing reloaded the graph is the build's: only a file just
        // recorded may join, against it.
        commands = refresh_provisional();
        if(commands.empty()) {
            return {};
        }
    }
    // Every borrowed or synthesized command may resolve differently now —
    // which no delta can tell.
    commands_epoch += 1;
    auto graph = rebuild_dependency_graph();
    commands.append(graph.provisional);

    // The compile command is an input that content-based staleness cannot
    // see. A unit the build stopped compiling — its database still loads but
    // no longer lists it, and no rule's default command covers it — leaves
    // the index and nothing is owed, unlike a file that vanished from disk,
    // whose shard stays behind (queries withhold it). A unit whose command
    // changed keeps its index serving until the reindex merges; its manifest
    // names the old command, so no freshness check skips the reindex, in
    // this session or after a restart. ContentChanged: a new command can
    // rewrite the index (macros, includes) as thoroughly as an edit.
    Cascade cascade{*this, open};
    for(auto file: llvm::concat<const Fid>(commands.added, commands.changed, commands.removed)) {
        if(candidates(file).empty()) {
            index::IndexReport report;
            index.drop(file, report);
            take_report(report);
            background_index.forget(file);
        } else {
            index.mark_outdated(file);
            cascade.reindex(file, true);
        }
    }

    // A module name that just gained its first provider: its unresolved-import
    // node's dependents are the TUs that scanned it unresolved — the delta
    // walk below cannot reach them (they hold no edge to any real node). A
    // name whose providers changed: importers hold edges to the ones they
    // picked, and when those entries are unchanged the delta walk cannot
    // reach them either — cascade from them so their next attempts pick
    // again.
    for(auto& name: graph.appeared) {
        cascade.provider_appeared(name);
    }
    for(auto replaced: graph.replaced) {
        cascade.invalidate_module(replaced);
    }

    // A module unit's command change invalidates importers' PCMs (no-op for
    // files the compile graph doesn't know). PCH/PCM keys embed the
    // canonical flags, so pull-side caches miss naturally.
    for(auto file: llvm::concat<const Fid>(commands.added, commands.changed, commands.removed)) {
        background_index.forget_skipped_diagnostics(file);
        cascade.invalidate_module(file);
    }
    cascade.affected.commands = std::move(commands);
    return std::move(cascade.affected);
}

}  // namespace clice

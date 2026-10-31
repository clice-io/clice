module clice;

import :index.project_index;
import :project.command_snapshot;
import :project.contexts;
import :sched.builders.pcm;
import :server.client;
import :server.master;
import :server.served_project;
import :support.logging;
import :vfs.file_system;
import :vfs.file_table;
import :vfs.path;
import :worker.probe;

namespace clice {

namespace {

/// The git directory of the checkout `root` lies in, if any: `.git`
/// itself, or the directory a linked worktree's `.git` file names.
std::optional<std::string> git_dir(llvm::StringRef root) {
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

/// The files whose default entry moved between two rankings, into
/// `changed`.
void push_moved(llvm::ArrayRef<Fid> files,
                llvm::ArrayRef<std::optional<SourceID>> before,
                llvm::ArrayRef<std::optional<SourceID>> after,
                llvm::SmallVectorImpl<Fid>& changed) {
    for(auto [file, was, now]: views::zip(files, before, after)) {
        if(was != now && !ranges::contains(changed, file)) {
            changed.push_back(file);
        }
    }
}

}  // namespace

bool ServedProject::compiled_by_session(Fid path_id) const {
    auto* document = find(path_id);
    return !project.settings.project.skip_header_bodies && document &&
           document->serving == ServingMode::Escalated;
}

void ServedProject::mark_stale(Fid path_id, bool& refresh) {
    if(auto* document = find(path_id)) {
        inputs_changed(*document->session);
        refresh = refresh || document->serving == ServingMode::Escalated;
    }
}

void ServedProject::reindex(Fid path_id, bool content) {
    skipped.erase(path_id);
    sched.pump.enqueue(path_id, content ? ReindexReason::ContentChanged : ReindexReason::DepsOnly);
}

/// An invalidated dependent recompiles when it is open, and reindexes
/// unless its document's compile serves it.
void ServedProject::mark_dependent(Fid path_id, bool& refresh) {
    project.index.mark_behind(path_id);
    mark_stale(path_id, refresh);
    if(!compiled_by_session(path_id)) {
        reindex(path_id, false);
    }
}

llvm::SmallVector<Fid> ServedProject::readers(Fid path_id) const {
    auto result = project.dep_graph.find_readers(path_id);
    auto add = [&](Fid reader) {
        if(reader != path_id && !ranges::contains(result, reader)) {
            result.push_back(reader);
        }
    };
    project.index.for_each_contribution(path_id, [&](Fid tu, std::uint64_t) { add(tu); });
    project.index.for_each_absent_lookup(path_id, add);
    project.index.for_each_importer(path_id, add);
    auto names = [&](const DepsSnapshot& deps) {
        return ranges::any_of(deps, [&](const DepState& dep) { return dep.path_id == path_id; });
    };
    for(auto& [document, served]: documents) {
        bool read = false;
        each_input(document, [&](const DepsSnapshot& deps) { read = read || names(deps); });
        if(read) {
            add(document);
        }
    }
    for(auto& [unit, build]: project.pcm_builds) {
        if(names(build.deps)) {
            add(unit);
        }
    }
    project.failures.pcm_builds.for_each([&](Fid unit, const auto& failed) {
        if(names(failed.trace)) {
            add(unit);
        }
    });
    return result;
}

void ServedProject::mark_readers(llvm::ArrayRef<Fid> readers, bool& refresh) {
    for(auto reader: readers) {
        cascade_compile_graph(reader, refresh);
        mark_dependent(reader, refresh);
    }
}

void ServedProject::cascade_compile_graph(Fid path_id, bool& refresh) {
    if(!sched.pcm.tracks(path_id)) {
        return;
    }
    for(auto dirtied: sched.pcm.invalidate(path_id)) {
        mark_dependent(dirtied, refresh);
    }
}

void ServedProject::provider_appeared(llvm::StringRef module_name, bool& refresh) {
    // Every consumer whose scan met the name unresolved holds a durable
    // edge to its sentinel; the graph cascade is the complete list — no
    // side bookkeeping, no reverse-map walk. Their shards lack the
    // module's symbols and their dep snapshots never named the
    // interface, so the content-hash gate would filter a DepsOnly
    // reindex — ContentChanged bypasses it. Nothing is dropped: a
    // rebuild replaces the shards, and a unit that can no longer build keeps
    // its last-known one (queries withhold a deleted file's).
    for(auto id: sched.pcm.provider_appeared(module_name)) {
        if(PCMBuilder::is_unresolved(id)) {
            continue;
        }
        auto path_id = Fid{static_cast<std::uint32_t>(id.key)};
        if(id.family == Family::TURun) {
            reindex(path_id, true);
        } else if(id.family == Family::AST) {
            if(find(path_id)) {
                mark_stale(path_id, refresh);
                if(!compiled_by_session(path_id)) {
                    reindex(path_id, true);
                }
            }
        } else {
            // A dirtied module unit: its importers are in this same list.
            mark_dependent(path_id, refresh);
        }
    }
}

void ServedProject::rescan_disk_state(Fid path_id, bool& refresh) {
    std::string old_module(project.dep_graph.module_of(path_id));
    project.rescan_disk_file(path_id);
    auto new_module = project.dep_graph.module_of(path_id);
    if(new_module == old_module) {
        return;
    }

    // A rescan that introduced a module declaration may have given the
    // name its first provider: consumers that scanned it unresolved hold
    // edges to its sentinel, not to any real node a module-graph cascade
    // could reach.
    if(!new_module.empty()) {
        if(project.dep_graph.lookup_module(new_module).size() == 1) {
            provider_appeared(new_module, refresh);
        }
        cascade_providers(new_module, path_id, refresh);
    }

    // The dropped name's consumers hold edges to this provider's real
    // node, and their builds embed a module the file no longer declares.
    // Nothing else on the close path finds this: an evicted PCM leaves
    // no cache entry for its staleness check to see.
    if(!old_module.empty()) {
        cascade_compile_graph(path_id, refresh);
        cascade_providers(old_module, path_id, refresh);
    }
}

void ServedProject::cascade_providers(llvm::StringRef module_name, Fid except, bool& refresh) {
    for(auto provider:
        ranges::to<llvm::SmallVector<Fid>>(project.dep_graph.lookup_module(module_name))) {
        if(provider != except) {
            cascade_compile_graph(provider, refresh);
        }
    }
}

void ServedProject::cascade_disk_content_change(Fid path_id, bool& refresh) {
    // Taken before the rescan below, which rewrites only the file's own
    // outgoing edges, never the includers this walks. A file the scan did
    // not know is new: its readers looked for it and failed, and their
    // rescans give the lexical graph the edges it lacked.
    auto dependents = readers(path_id);
    if(!project.dep_graph.knows(path_id)) {
        for(auto reader: dependents) {
            rescan_disk_state(reader, refresh);
        }
    }

    // Rescan disk state (include edges, module declaration); then cascade
    // through the module graph — importers' build products went stale, and
    // the cascade names every affected module unit.
    rescan_disk_state(path_id, refresh);
    cascade_compile_graph(path_id, refresh);

    // The new content is an input of every unit that read it: open ones
    // recompile, closed ones reindex so cross-file references stop serving
    // the stale state, module units check their PCM again. Enqueueing is
    // O(1) per TU and deliberately uncapped — the index's content-hash
    // staleness check filters TUs whose dependencies did not actually
    // change, and the idle/priority scheduling throttles the rest.
    mark_readers(dependents, refresh);
}

void ServedProject::disk_changed(llvm::ArrayRef<Fid> files) {
    bool refresh = false;
    for(auto path_id: files) {
        if(!project.file_table.disk.seen_missing(path_id)) {
            // Whether or not a buffer is open: the disk is what every
            // other file compiles against and what the index describes.
            cascade_disk_content_change(path_id, refresh);
            // A unit of its own reads the file as well.
            project.index.mark_behind(path_id);
            reindex(path_id, true);
            continue;
        }
        // Dependents compile against a now-missing include: open ones
        // recompile (the missing-file diagnostic is the truth), closed ones
        // reindex — nothing else would ever queue them. Taken before
        // forget_file below rewrites the graph.
        mark_readers(readers(path_id), refresh);
        // A removed module unit takes its PCM with it: importers' build
        // products went stale.
        cascade_compile_graph(path_id, refresh);
        cascade_providers(std::string(project.dep_graph.module_of(path_id)), path_id, refresh);
        // Any reindex reason recorded before the removal — e.g. a change
        // observed moments earlier — is dropped: there is nothing to
        // reindex any more, and the file's shard stays behind (queries
        // withhold the shard of a file seen missing). After the
        // compile-graph cascade, which lists the removed module itself
        // among its dirtied units: the removal is the final word for the
        // file itself.
        sched.pump.clear_pending(path_id);
        project.forget_file(path_id);
        // TODO: remove orphaned shards of files that stay deleted.
    }
    finish_changes(refresh);
}

void ServedProject::build_changed(CDBDiff delta) {
    bool refresh = false;
    // The lender set changed: every borrowed or synthesized command may
    // resolve differently now — which no delta can tell, so all of them
    // recompile.
    project.commands_epoch += 1;
    for(auto& [path_id, document]: documents) {
        if(document.guessed) {
            mark_stale(path_id, refresh);
        }
    }
    // With nothing reloaded the graph is the build's: only a file just
    // recorded may join, against it.
    if(delta.empty()) {
        delta = project.refresh_provisional();
        if(delta.empty()) {
            finish_changes(refresh);
            return;
        }
    }

    // The database already reloaded; derived state must follow. The
    // provisional members join, change command or leave the way an entry
    // does.
    auto providers = project.rebuild_dependency_graph();
    delta.append(providers.provisional);

    // A module name that just gained its first provider: its sentinel's
    // dependents are the TUs that scanned it unresolved — the delta walk
    // below cannot reach them (they hold no edge to any real node). A name
    // whose providers changed: importers hold edges to the ones they
    // picked, and when those entries are unchanged the delta walk cannot
    // reach them either — cascade from them so their next rounds pick
    // again.
    for(auto& name: providers.appeared) {
        provider_appeared(name, refresh);
    }
    for(auto replaced: providers.replaced) {
        cascade_compile_graph(replaced, refresh);
    }

    // Every delta entry needs the same treatment — the compile command is
    // an input that content-based staleness cannot see, whether it
    // appeared, changed or vanished. PCH/PCM keys embed the canonical
    // flags, so pull-side caches miss naturally.
    auto invalidate_entry = [&](Fid path_id, bool retired) {
        // The next compile re-resolves the command (added: first real entry
        // replaces the guessed one; changed: new flags; removed: fall back).
        mark_stale(path_id, refresh);
        // A TU the build stopped compiling — its database still loads but no
        // longer lists it: its shard leaves the index and nothing is owed,
        // unlike a file that vanished from disk, whose shard stays behind
        // (queries withhold it). A TU whose command changed: its index
        // describes a compile that no longer exists, and content-based
        // freshness cannot see that — a surviving manifest would judge the
        // queued reindex fresh and keep the old-command index serving, in
        // this session and after a restart. ContentChanged: a new command
        // can rewrite the index (macros, includes) as thoroughly as an edit.
        index::IndexReport report;
        project.index.drop(path_id, report);
        sched.pump.apply_report(report);
        if(retired) {
            sched.pump.clear_pending(path_id);
        } else {
            reindex(path_id, true);
        }

        // A module unit's command change invalidates importers' PCMs
        // (no-op for files the compile graph doesn't know).
        cascade_compile_graph(path_id, refresh);

        // An open header borrowing the changed command compiled under one
        // that no longer exists; one whose first compile is still in
        // flight may be borrowing it.
        for(auto& [header, document]: documents) {
            if(document.latest ? document.latest->host == path_id : !project.build.unit(header)) {
                mark_stale(header, refresh);
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
        // The database that owned the entry reloaded fine and no longer
        // lists the file: the build stopped compiling it — unless a rule's
        // default command still covers it, which makes this a command
        // change. (A database that vanishes keeps serving its entries — a
        // missing file never reloads — so this is not a removal from
        // disk.) The graph rebuild above already dropped a retired file's
        // source role, and the choices' recheck sees it.
        invalidate_entry(path_id, /*retired=*/project.build.commands(path_id).empty());
    }
    finish_changes(refresh);

    // The database gained or lost entries: an open document may belong to
    // another project now.
    if(!delta.added.empty() || !delta.removed.empty()) {
        server.reroute();
    }
}

void ServedProject::finish_changes(bool refresh) {
    // A change can remove the include edge or the entry a user's context
    // choice depends on, or bring it back.
    recheck_choices();
    // Not before the server is ready: document-sync events are accepted
    // early, but the scheduler reads settings that initialize() has not
    // applied yet. The queue filled is kept — the project's start kicks the
    // scheduler.
    if(server.lifecycle == ServerLifecycle::Ready) {
        sched.pump.schedule();
    }
    if(refresh && server.client) {
        server.client->refresh_diagnostics();
        server.client->refresh();
    }
}

void ServedProject::recheck_choices() {
    bool rerouted = false;
    for(auto& [file, entry]: server.choices) {
        if(entry.project != this || !server.recheck_choice(file)) {
            continue;
        }
        rerouted = true;
        // Whether the choice holds is part of what the document compiles
        // under.
        if(auto* document = find(file)) {
            choice_changed(*document->session);
        }
    }
    if(rerouted) {
        server.reroute();
    }
}

void ServedProject::asts_lost(llvm::ArrayRef<Fid> lost) {
    // Compile inputs did not change, so trial state and self-containment
    // results stay untouched. A live round sends a compile of its own,
    // which puts the AST back on a worker — invalidating it would only make
    // it finish stale and compile twice.
    for(auto path_id: lost) {
        if(find(path_id) && !compiling(path_id)) {
            invalidate(path_id);
        }
    }
}

bool ServedProject::knows(Fid path_id) {
    if(broken()) {
        return false;
    }
    return find(path_id) != nullptr || !project.build.commands(path_id).empty() ||
           project.dep_graph.knows(path_id) || !readers(path_id).empty();
}

void ServedProject::open_closures(llvm::SmallVectorImpl<Fid>& files) const {
    for(auto& [path_id, document]: documents) {
        each_input(path_id, [&](const DepsSnapshot& deps) {
            for(auto& dep: deps) {
                files.push_back(dep.path_id);
            }
        });
    }
}

bool ServedProject::databases_moved() const {
    return resolved_databases(project.settings, project.build.active_configuration()) !=
           project.resolved_databases;
}

std::size_t ServedProject::reload_databases(bool force) {
    if(!watching) {
        return 0;
    }
    // A reload would record the other build's commands as the ones this
    // index was built with.
    if(databases_moved()) {
        server.ask_restart({
            .file = settings_file,
            .message = std::format("The compilation databases of {} now resolve to another build",
                                   project.file_table.display(project.root)),
        });
        return 0;
    }
    if(!project.build.declares_sources()) {
        discover_databases();
    }
    CDBDiff delta;
    for(auto& database: watched) {
        tick_database(database, force, delta);
    }
    if(delta.empty()) {
        return 0;
    }
    build_changed(std::move(delta));
    return 1;
}

void ServedProject::tick_databases() {
    if(project.settings.tracker.workspace_poll_seconds.value > 0 && reload_databases(false) > 0) {
        server.probe.happened(LoopEvent::DatabaseChange);
    }
}

kota::task<bool> ServedProject::poll_sources() {
    if(!watching) {
        co_return false;
    }
    auto walked =
        co_await kota::queue([walk = project.build.source_walk()] { return walk_sources(walk); });
    auto added = project.build.refresh_default_sources(walked);
    if(added.empty()) {
        co_return false;
    }
    build_changed({.added = std::move(added)});
    co_return true;
}

void ServedProject::discover_around(Fid path_id) {
    if(!watching || project.build.declares_sources() || !project.build.entries(path_id).empty()) {
        return;
    }
    auto path = project.file_table.resolve(path_id);
    if(!path::is_within(path, project.root)) {
        return;
    }
    CDBDiff found;
    // A registered database whose load failed so far (absent at startup,
    // unreadable at an earlier open) gets another try: with polling off
    // nothing else would.
    for(auto& database: compile_commands_above(path.parent(), project.root)) {
        auto registered = project.cdb.find_source(database);
        if((registered && project.cdb.loaded(*registered)) ||
           (!registered && project.cdb.find_same_file(database))) {
            continue;
        }
        auto id = registered ? *registered : project.cdb.add_source(database);
        if(auto diff = project.cdb.reload_and_diff(id)) {
            LOG_INFO("Found compilation database: {}", database);
            found.append(*diff);
        }
        if(!registered) {
            watch_database(id);
        }
    }
    if(!found.empty()) {
        build_changed(std::move(found));
    }
}

void ServedProject::watch_checkout() {
    auto git = git_dir(project.root);
    if(!git) {
        return;
    }
    for(auto name: {"HEAD", "index"}) {
        checkout.push_back(project.file_table.disk.watch(
            path::join(*git, name),
            [&disk = project.file_table.disk, root = project.root.str()] {
                disk.expire_under(root);
            }));
    }
}

ServedProject::Hashes ServedProject::loaded_hashes(SourceID id) const {
    return ranges::to<Hashes>(
        views::transform(project.cdb.inputs(id), [](auto& input) { return input.hash; }));
}

void ServedProject::watch_inputs(WatchedDatabase& database) {
    database.inputs.truncate(1);
    for(auto& input: project.cdb.inputs(database.id).drop_front()) {
        database.inputs.push_back(
            project.file_table.disk.watch(project.file_table.resolve(input.file).str()));
    }
}

void ServedProject::watch_database(SourceID id) {
    auto& database = watched.emplace_back(WatchedDatabase{
        .id = id,
        .applied = loaded_hashes(id),
        .inputs = {project.file_table.disk.watch(project.cdb.source_path(id).str())},
    });
    watch_inputs(database);
}

llvm::SmallVector<Fid> ServedProject::shared_files(SourceID id) const {
    llvm::SmallVector<Fid> shared;
    for(auto group: project.cdb.entries() |
                        views::chunk_by([](const CompilationEntry& a, const CompilationEntry& b) {
                            return a.file == b.file;
                        })) {
        auto listed = [&](const CompilationEntry& entry) {
            return entry.source == id;
        };
        if(ranges::any_of(group, listed) && !ranges::all_of(group, listed)) {
            shared.push_back(group.front().file);
        }
    }
    return shared;
}

llvm::SmallVector<std::optional<SourceID>>
    ServedProject::default_sources(llvm::ArrayRef<Fid> files) const {
    return ranges::to<llvm::SmallVector<std::optional<SourceID>>>(
        views::transform(files, [&](Fid file) -> std::optional<SourceID> {
            auto entries = project.build.entries(file);
            if(entries.empty()) {
                return std::nullopt;
            }
            return entries.front().source;
        }));
}

void ServedProject::tick_database(WatchedDatabase& database, bool force, CDBDiff& delta) {
    auto current = ranges::to<Hashes>(
        views::transform(database.inputs, [](auto& input) { return input->hash; }));
    if(!force) {
        if(current == database.applied) {
            database.pending.reset();
            return;
        }
        if(database.pending != current) {
            // Generators rewrite the file in place; only act once the
            // content has held for two consecutive ticks (half-write guard).
            database.pending = std::move(current);
            return;
        }
    }
    // A forced tick reloads unconditionally: a spurious reload just yields
    // an empty diff.
    database.pending.reset();
    bool exists = database.inputs.front()->stamp.has_value();
    // A discovered database's presence ranks it (see Build::source_order):
    // the files whose default entry moves with it change command.
    bool flips =
        project.cdb.present(database.id) != exists && project.build.discovered(database.id);
    llvm::SmallVector<Fid> shared;
    llvm::SmallVector<std::optional<SourceID>> before;
    if(flips) {
        shared = shared_files(database.id);
        before = default_sources(shared);
    }
    if(!exists) {
        // Deleted — usually mid-regeneration. Keep serving the loaded
        // entries; the rewrite arrives as the next observed change.
        database.applied = std::move(current);
        project.cdb.set_present(database.id, false);
        if(flips) {
            push_moved(shared, before, default_sources(shared), delta.changed);
        }
        return;
    }
    auto diff = project.cdb.reload_and_diff(database.id);
    // The baseline is the reload's own reads: a rewrite arriving meanwhile
    // is seen next tick. A database the reload could not read (still
    // locked by the generator) leaves them as they were, so the reload is
    // retried; one it read but could not parse is retried once it moves.
    database.applied = loaded_hashes(database.id);
    watch_inputs(database);
    if(!diff) {
        return;
    }
    LOG_INFO("Reloaded CDB from {}: {} added, {} removed, {} changed",
             project.cdb.source_path(database.id),
             diff->added.size(),
             diff->removed.size(),
             diff->changed.size());
    if(flips) {
        push_moved(shared, before, default_sources(shared), diff->changed);
    }
    delta.append(*diff);
}

void ServedProject::discover_databases() {
    auto& disk = project.file_table.disk;
    if(!root_flag) {
        root_flag = disk.watch(project.root.str());
    }
    if(!listed_at || root_flag->stamp != listed_at) {
        listed = database_places(project.root);
        // An entry made within the clock tick of the root's last change
        // leaves its stamp as it was: only a stable stamp guarantees the
        // listing (see vfs::stable_mtime).
        auto& stamp = root_flag->stamp;
        listed_at = stamp && vfs::stable_mtime(stamp->mtime_ns) ? stamp : std::nullopt;
    }

    // In the order a startup discovery would register them, the nearest
    // database first above each open document.
    llvm::SmallVector<Spelling> wanted;
    llvm::StringSet<> seen;
    auto want = [&](llvm::ArrayRef<Spelling> candidates) {
        for(auto& place: candidates) {
            if(seen.insert(place).second) {
                wanted.push_back(place);
            }
        }
    };
    want(listed);
    for(auto& [path_id, document]: documents) {
        auto path = project.file_table.resolve(path_id);
        if(project.build.entries(path_id).empty() && path::is_within(path, project.root)) {
            want(database_places_above(path.parent(), project.root));
        }
    }

    llvm::SmallVector<std::string> unwanted;
    for(auto& entry: places) {
        if(!seen.contains(entry.getKey())) {
            unwanted.push_back(entry.getKey().str());
        }
    }
    for(auto& place: unwanted) {
        places.erase(place);
    }

    for(auto& place: wanted) {
        if(registered.contains(place)) {
            continue;
        }
        auto it = places.find(place);
        if(it == places.end()) {
            if(project.cdb.find_source(place)) {
                registered.insert(place);
                continue;
            }
            it = places.try_emplace(place, disk.watch(place.str())).first;
        }
        if(!it->second->stamp) {
            continue;
        }
        // Another spelling of a tracked database, as build/compile_commands.json
        // is when the root links it, stays watched: once the link goes, it
        // is a database of its own.
        if(project.cdb.find_same_file(place)) {
            continue;
        }
        // Never loaded, so baselined unread: the fresh file is a change
        // against the never-loaded source and goes through the normal
        // debounce-and-reload path.
        auto id = project.cdb.add_source(place);
        if(ranges::none_of(watched,
                           [&](const WatchedDatabase& database) { return database.id == id; })) {
            LOG_INFO("Found compilation database: {}", place);
            watch_database(id);
        }
        registered.insert(place);
        places.erase(it);
    }
}

}  // namespace clice

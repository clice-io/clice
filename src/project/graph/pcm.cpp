module clice;

import :project.command;
import :project.pch;
import :project.pcm;
import :project.precompiled;
import :project.project;
import :support.logging;
import :syntax.header_context;
import :syntax.preamble;
import :syntax.scan;
import :vfs.file_system;
import :vfs.file_table;
import :vfs.path;
import :worker.protocol;

namespace clice {

namespace {

/// Changes whenever a PCM build of the same command comes to write another
/// BMI, or its key comes to name another build, so a blob or a record an
/// older clice stored is not taken for the new one.
constexpr llvm::StringRef pcm_version = "5";

/// The provider an import in `importer` resolves to among several of one
/// name: the nearest — the fewest directories up from the importer to one
/// holding it — and the first by path among equally near ones.
Fid nearest_provider(const FileTable& files, llvm::ArrayRef<Fid> providers, Fid importer) {
    if(providers.size() == 1) {
        return providers.front();
    }
    auto distance = [&](Fid provider) {
        std::uint32_t up = 0;
        path::walk_ancestors(files.resolve(importer).parent(), [&](CanonicalRef dir) {
            up += 1;
            return !path::is_within(files.resolve(provider), dir);
        });
        return up;
    };
    return *ranges::min_element(providers, {}, [&](Fid provider) {
        return std::pair(distance(provider), llvm::StringRef(files.resolve(provider)));
    });
}

/// Move the build's PCM to `previous`, removing the one it replaced.
void move_to_previous(Project& project, PCMBuild& build) {
    if(build.blob.empty()) {
        return;
    }
    if(!build.previous.empty()) {
        project.store->invalidate("pcm", build.previous);
    }
    build.previous = std::exchange(build.blob, {});
}

/// The build's PCM, or the PCH it was built over, left the disk: forget it,
/// so the module's next attempt builds again. Returns whether it had.
bool drop_evicted(PCMBuild& build) {
    if(build.path.empty()) {
        return false;
    }
    if(!fs::exists(build.path)) {
        build.blob.clear();
    } else if(build.pch.empty() || fs::exists(build.pch)) {
        return false;
    }
    build.path.clear();
    return true;
}

/// The modules a compile importing `modules` reads, once it waited on them:
/// they and, below each that built, the modules it imports in turn — clang
/// loads every module below an import, and a built module's imports are
/// built too.
llvm::SetVector<Fid> module_closure(const Project& project, llvm::ArrayRef<Fid> modules) {
    llvm::SetVector<Fid> read(modules.begin(), modules.end());
    for(std::size_t i = 0; i < read.size(); i += 1) {
        auto it = project.pcm_builds.find(read[i]);
        if(it == project.pcm_builds.end() || it->second.path.empty()) {
            continue;
        }
        for(auto dep: project.graph.dependencies(module_node(read[i]))) {
            if(dep.kind == NodeKind::PCM && !is_unresolved_import(dep)) {
                read.insert(dep.file());
            }
        }
    }
    return read;
}

/// Why the module's last build cannot stand for `pcm_key`; empty when it
/// can: built under the key, what it read unchanged, its PCM in the store
/// and on disk.
/// A build that cannot no longer describes the module: an importer
/// compiling while the next one runs, or after it fails, must not read the
/// previous interface from it. Its PCM stays in the store until the next
/// one replaces it — a compile may still be reading it.
llvm::StringRef revalidate(Project& project, Fid module, llvm::StringRef pcm_key) {
    auto it = project.pcm_builds.find(module);
    if(it == project.pcm_builds.end()) {
        return "no_entry";
    }
    auto& build = it->second;
    llvm::StringRef miss;
    if(build.key != pcm_key) {
        miss = "key_changed";
    } else if(deps_changed(project.file_table, build.deps)) {
        miss = "deps_changed";
    } else if(build.path.empty() || !project.store->lookup("pcm", build.blob) ||
              !fs::exists(build.path)) {
        miss = "evicted";
    } else {
        return {};
    }
    build = {.blob = std::move(build.blob), .previous = std::move(build.previous)};
    return miss;
}

/// Whether the module's last build stands failed against building it again
/// under `pcm_key`. A failed build rebuilt on unchanged inputs would make
/// every compile and completion of every importer pay for it first, and one
/// that crashed a worker would burn a worker of every importer. A crash's
/// inputs hold the module's own text, which pcm_key leaves out: the refusal
/// lifts the moment the poison is edited, and only then — a rebuild of the
/// PCH renames the key on the same bytes. A failure under another key that
/// crashed nothing no longer holds, and is forgotten.
bool refused(Project& project, Fid module, llvm::StringRef pcm_key, llvm::StringRef name) {
    auto* failed = project.failures.pcm_builds.lookup(project.file_table, module);
    if(!failed) {
        return false;
    }
    if(failed->result.record.crashed()) {
        LOG_WARN("PCM build for module {} refused: it crashed a worker", name);
        return true;
    }
    if(failed->result.key == pcm_key) {
        LOG_DEBUG("PCM build for module {} skipped: it failed on these inputs", name);
        return true;
    }
    project.failures.pcm_builds.forget(module);
    return false;
}

/// One attempt of a module's PCM: declare the module's imports as its
/// edges, wait on them, keep the last build while it holds, else dispatch a
/// new one.
async::task<Result> build_module(Project& project, Attempt& attempt, Fid path_id) {
    // Inputs that moved may import otherwise: a scan taken before is no
    // answer for them, even while no event has reached the disk state yet.
    if(auto* inputs = project.build_inputs(path_id);
       inputs && deps_changed(project.file_table, *inputs)) {
        project.disk_scans.erase(path_id);
    }

    // The import list is scanner truth, not build output: commit it as
    // the node's edges before building, so a build that fails stays
    // reachable from its imports — fixing or providing one re-dirties it.
    // The scan runs under the command the build compiles with, resolved on
    // every attempt: a CDB or import change is always seen by the next one.
    worker::BuildPCMParams bp;
    auto resolution = project.resolve_command(path_id, bp.directory, bp.arguments);
    bp.hidden = resolution.hidden;
    auto deps =
        co_await project.direct_deps(path_id, resolution, bp.arguments, bp.directory, std::nullopt);
    project.graph.set_edges(attempt.node(), deps.declared);

    // A unit of a target with precompiled headers builds over clice's own
    // build of them, as its command builds over the target's: the header
    // it forces in then reads nothing again, where parsed as text its
    // declarations would precede the module declaration. The PCM names
    // that PCH to its importers, which read it with the PCM: it is the
    // index's, keeping the function bodies, for every importer to read the
    // same one. One that did not build leaves the header to the unit's own
    // parse, as for any other unit.
    std::optional<std::string> pch_key;
    if(auto unit = project.pch_unit(path_id, resolution)) {
        auto request = project.declared_pch_request(*unit, /*skip_bodies=*/false);
        auto key = request.pch_key;
        switch(co_await attempt.depend(pch_node(key), project.pch_work(std::move(request)))) {
            case Result::Success: pch_key = std::move(key); break;
            case Result::Failed: break;
            case Result::Stale: co_return Result::Stale;
        }
    }

    // The last attempt's failed import stands while this one waits: an
    // importer explains its diagnostics from it meanwhile.
    auto resolved = deps.resolved();
    for(int pass = 0;; pass += 1) {
        for(auto dep: resolved) {
            switch(co_await attempt.depend(module_node(dep), project.module_work(dep))) {
                case Result::Success: break;
                case Result::Failed: {
                    auto& build = project.pcm_builds[path_id];
                    move_to_previous(project, build);
                    build = {.previous = std::move(build.previous)};
                    project.failures.pcm_builds.commit(path_id, {.import = dep}, {});
                    co_return Result::Failed;
                }
                case Result::Stale: co_return Result::Stale;
            }
        }
        if(project.imports_present(resolved)) {
            break;
        }
        // A build now would fail on the missing PCM and stand refused on
        // inputs that never failed it; failing unrecorded, the next wait
        // tries again.
        if(pass + 1 == import_passes) {
            LOG_WARN("PCM build for {} skipped: an import's PCM keeps leaving the cache",
                     project.file_table.resolve(path_id));
            co_return Result::Failed;
        }
    }

    // Copied before any suspension below: while a PCM build is awaited, a
    // concurrent didSave can re-declare the file and drop the graph's
    // string.
    std::string module_name(project.dep_graph.module_of(path_id));
    if(module_name.empty()) {
        co_return Result::Failed;
    }
    auto file_path = std::string(project.file_table.resolve(path_id));
    bp.file = file_path;

    if(!project.store) {
        LOG_WARN("PCM build skipped for module {}: cache store is unavailable", module_name);
        co_return Result::Failed;
    }

    // The PCH as the build starts, like the imports.
    std::string pch_blob;
    DepsSnapshot precompiled;
    if(pch_key) {
        if(auto it = project.pch_cache.find(*pch_key);
           it != project.pch_cache.end() && !it->second.path.empty()) {
            bp.pch = it->second.path;
            pch_blob = it->second.blob;
            precompiled = it->second.deps;
        }
    }

    // The key names everything of the build but file content, which the
    // inputs cover: the source path, the frontend-relevant subset of the
    // compile flags, the index database that records the blob's
    // dependency stamps, what each import resolved to — the keys of the
    // imported modules, which name theirs in turn — and the very build of
    // the PCH it reads: rebuilt, that one has another name.
    std::string imports;
    for(auto dep: deps.declared) {
        if(is_unresolved_import(dep)) {
            imports += std::format("?{:x}", dep.key);
        } else {
            imports += project.pcm_builds.find(dep.file())->second.key;
        }
        imports.push_back('\0');
    }
    auto safe_module_name = module_name;
    ranges::replace(safe_module_name, ':', '-');
    auto pcm_key = std::format("{}-{}",
                               safe_module_name,
                               cache_key({clang::getClangFullVersion(),
                                          pcm_version,
                                          project.database_key.str(),
                                          bp.directory,
                                          file_path,
                                          canonicalize(bp.arguments),
                                          imports,
                                          pch_blob}));

    auto pcm_miss = revalidate(project, path_id, pcm_key);
    if(pcm_miss.empty()) {
        LOG_PERF("cache", "ns=pcm event=hit key={} module={}", pcm_key, module_name);
        // A record read back from the database names no PCH.
        project.pcm_builds.find(path_id)->second.pch = std::move(bp.pch);
        co_return Result::Success;
    }
    LOG_PERF("cache",
             "ns=pcm event=miss reason={} key={} module={}",
             pcm_miss,
             pcm_key,
             module_name);

    if(refused(project, path_id, pcm_key, module_name)) {
        co_return Result::Failed;
    }

    bp.module_name = module_name;
    auto blob = new_blob(pcm_key);
    auto pending = project.store->begin_store("pcm", blob);
    bp.output_path = pending.tmp_path;
    bp.pcms = project.imported_pcms(resolved);

    // The modules it imports are inputs as much as its own text — the PCM
    // embeds what it read of them — and theirs already carry their own
    // imports', so the snapshot is transitive. Taken before the build, with
    // the PCH it reads: an eviction arriving during it must not drop them.
    auto before = project.module_inputs(resolved);
    merge_deps(before, {&precompiled});
    // What a build that crashes is known to read, sampled before it runs: a
    // save arriving mid-build is not what crashed.
    auto known = before;
    if(auto own = current_dep(project.file_table, path_id)) {
        known.push_back(*own);
    }
    auto built = co_await project.send_precompiled(
        attempt,
        bp,
        std::format("module {}", module_name),
        std::move(known),
        before,
        [&](FailureRecord record, DepsSnapshot inputs) {
            // An attempt overtaken in flight records its inputs
            // all the same: they no longer match, and its
            // successor rebuilds.
            project.failures.pcm_builds.commit(path_id,
                                               {.key = pcm_key, .record = std::move(record)},
                                               std::move(inputs));
        });
    if(!built) {
        if(built.error() == Result::Failed) {
            move_to_previous(project, project.pcm_builds[path_id]);
        }
        co_return built.error();
    }

    // Commit on the thread pool: it fsyncs the freshly written PCM.
    auto committed =
        co_await async::queue([&] { return project.store->commit(std::move(pending)); });
    auto& build = project.pcm_builds[path_id];
    move_to_previous(project, build);
    if(!committed.has_value()) {
        LOG_WARN("Failed to commit PCM for module {}", module_name);
        co_return Result::Failed;
    }
    build.key = std::move(pcm_key);
    build.blob = std::move(blob);
    build.path = std::move(committed.value());
    build.pch = std::move(bp.pch);
    build.deps = project.read_inputs(*built, before);
    LOG_INFO("PCM built for module {}: {}", module_name, build.path);

    project.mark_precompiled_dirty();

    co_return Result::Success;
}

}  // namespace

bool Project::imports_present(llvm::ArrayRef<Fid> modules) {
    bool present = true;
    for(auto module: module_closure(*this, modules)) {
        auto it = pcm_builds.find(module);
        if(it == pcm_builds.end() || drop_evicted(it->second) || it->second.path.empty()) {
            present = false;
        }
    }
    return present;
}

llvm::SmallVector<Fid> ModuleDeps::resolved() const {
    llvm::SmallVector<Fid> modules;
    for(auto dep: declared) {
        if(!is_unresolved_import(dep)) {
            modules.push_back(dep.file());
        }
    }
    return modules;
}

bool ModuleDeps::modular() const {
    return module_unit ||
           ranges::any_of(declared, [](NodeId dep) { return !is_unresolved_import(dep); });
}

async::task<ModuleDeps> Project::direct_deps(Fid path_id,
                                             const Resolution& resolution,
                                             llvm::ArrayRef<std::string> arguments,
                                             llvm::StringRef directory,
                                             std::optional<llvm::StringRef> content) {
    bool may_import = dep_graph.reaches_import(path_id) ||
                      (resolution.host.valid() && dep_graph.reaches_import(resolution.host));
    std::uint64_t directives = 0;
    std::uint32_t bound = 0;
    if(content) {
        // Directives the scan never saw may include anything, so past the
        // graph only a project without module syntax is sure.
        auto lexical = scan_quick(*content);
        may_import |=
            lexical.has_module_syntax() || (dep_graph.has_import_candidates() &&
                                            !dep_graph.scanned(path_id, lexical.directives_hash));
        directives = lexical.directives_hash;
        bound = preamble_bound(*content);
    }
    if(!may_import) {
        co_return ModuleDeps{};
    }

    llvm::SmallString<1024> joined(directory);
    for(auto& arg: arguments) {
        joined.push_back('\0');
        joined.append(arg);
    }
    auto arguments_hash = llvm::xxh3_64bits(joined);
    auto epoch = context_epoch;

    auto& scans = content ? buffer_scans : disk_scans;
    ImportScan scan;
    if(auto memo = scans.find(path_id);
       memo != scans.end() && memo->second.directives == directives &&
       memo->second.bound == bound && memo->second.arguments == arguments_hash &&
       memo->second.epoch == epoch) {
        scan = memo->second;
    } else {
        llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> vfs =
            llvm::makeIntrusiveRefCnt<vfs::View>(resolution.hidden);
        if(auto& synthesized = resolution.synthesized) {
            auto memory = llvm::makeIntrusiveRefCnt<llvm::vfs::InMemoryFileSystem>();
            for(auto& file: synthesized->files) {
                memory->addFile(file.path,
                                0,
                                llvm::MemoryBuffer::getMemBufferCopy(file.content, file.path));
            }
            auto overlay = llvm::makeIntrusiveRefCnt<llvm::vfs::OverlayFileSystem>(vfs);
            overlay->pushOverlay(std::move(memory));
            vfs = std::move(overlay);
        }
        // The compile reads the text gone on with the include of a header
        // context's suffix (CompilationInput::suffix), which may import.
        std::string text;
        if(auto suffix = resolution.synthesized ? suffix_path(resolution.synthesized->files)
                                                : llvm::StringRef();
           content && !suffix.empty()) {
            text = content->str();
            append_include(text, suffix);
            content = text;
        }
        import_scans += 1;
        std::vector<const char*> argv;
        argv.reserve(arguments.size());
        for(auto& arg: arguments) {
            argv.push_back(arg.c_str());
        }
        auto scanned = co_await async::queue(
            [&] { return scan_precise(argv, directory, content, std::move(vfs)); });
        scan = {.directives = directives,
                .bound = bound,
                .arguments = arguments_hash,
                .epoch = epoch,
                .imports = std::move(scanned.modules),
                .offsets = std::move(scanned.import_offsets),
                .module_unit = !scanned.module_name.empty()};
        // An implementation unit imports its own module implicitly.
        if(scanned.is_implementation_unit()) {
            scan.imports.push_back(std::move(scanned.module_name));
            scan.offsets.push_back(std::numeric_limits<std::uint32_t>::max());
        }
        scans[path_id] = scan;
    }

    // Every scanned name goes into the edge set, resolved or not: an
    // unresolved name edges to its unresolved-import node, which is what
    // lets the name's first provider re-dirty this unit through the
    // ordinary cascade later — no side bookkeeping of who failed against it.
    ModuleDeps deps{
        .offsets = {scan.offsets.begin(), scan.offsets.end()},
        .module_unit = scan.module_unit
    };
    for(auto& name: scan.imports) {
        auto providers = dep_graph.lookup_module(name);
        deps.declared.push_back(
            providers.empty() ? unresolved_import(name)
                              : module_node(nearest_provider(file_table, providers, path_id)));
    }
    co_return deps;
}

llvm::SmallVector<NodeId> Project::provider_appeared(llvm::StringRef name) {
    return graph.invalidate(unresolved_import(name));
}

Work Project::module_work(Fid module) {
    return {
        .run = [this, module](Attempt& attempt) { return build_module(*this, attempt, module); },
        .check = [this,
                  module] { return imports_present(module) ? Check::Holds : Check::ProductGone; },
    };
}

std::optional<ModuleBuildFailure> Project::module_build_failure(Fid module) const {
    ModuleBuildFailure failure;
    for(auto current = module;;) {
        if(auto seen = ranges::find(failure.chain, current); seen != failure.chain.end()) {
            failure.cycle = seen - failure.chain.begin();
            return failure;
        }
        failure.chain.push_back(current);
        auto* failed = failures.pcm_builds.find(current);
        if(!failed || failed->result.record.crashed()) {
            return std::nullopt;
        }
        if(!failed->result.import.valid()) {
            failure.error = failed->result.record.cause;
            return failure;
        }
        current = failed->result.import;
    }
}

bool Project::forget_build_failure(Fid module) {
    return failures.pcm_builds.forget(module);
}

const FailureRecord* Project::build_crash(Fid module) {
    // FIXME: what a crash is known to read leaves out the headers the module
    // includes: a fix there waits for an importer's edit or save, and its
    // importers fail to index until then.
    auto* failed = failures.pcm_builds.lookup(file_table, module);
    return failed && failed->result.record.crashed() ? &failed->result.record : nullptr;
}

async::task<std::optional<ModuleDeps>> Project::build_imports(NodeId consumer,
                                                              Fid path_id,
                                                              const Resolution& resolution,
                                                              llvm::ArrayRef<std::string> arguments,
                                                              llvm::StringRef directory,
                                                              llvm::StringRef content) {
    // Resolved fresh on every call — a stale list must never outlive a
    // CDB change. The consumer's edges must live in the graph: a saved
    // module (or a provider appearing for an unresolved import) cascades to
    // the open TUs importing it through them. Declared even when empty, so
    // a removed import stops cascading.
    auto deps = co_await direct_deps(path_id,
                                     resolution,
                                     arguments,
                                     directory,
                                     std::optional<llvm::StringRef>(content));
    graph.set_edges(consumer, deps.declared);
    // A user request waits on these builds: foreground, so the background
    // budget cannot throttle them. A module that did not build is the
    // compile's to report; waiting through stale attempts, only a shutdown
    // ends one Stale.
    auto resolved = deps.resolved();
    for(int pass = 0; pass < import_passes; pass += 1) {
        std::vector<async::task<Result>> waits;
        for(auto dep: resolved) {
            waits.push_back(graph.join(module_node(dep),
                                       module_work(dep),
                                       {.foreground = true, .retry = [] { return true; }}));
        }
        auto results = co_await async::when_all(std::move(waits));
        if(ranges::contains(results, Result::Stale)) {
            co_return std::nullopt;
        }
        if(imports_present(resolved)) {
            break;
        }
    }
    co_return deps;
}

const DepsSnapshot* Project::build_inputs(Fid module) const {
    llvm::SmallVector<Fid, 4> chain;
    for(auto current = module; !ranges::contains(chain, current);) {
        auto* failed = failures.pcm_builds.find(current);
        if(!failed) {
            auto it = pcm_builds.find(current);
            return it != pcm_builds.end() ? &it->second.deps : nullptr;
        }
        if(!failed->result.import.valid()) {
            return &failed->trace;
        }
        chain.push_back(current);
        current = failed->result.import;
    }
    return nullptr;
}

DepsSnapshot Project::module_inputs(llvm::ArrayRef<Fid> modules) const {
    llvm::SmallVector<const DepsSnapshot*> parts;
    for(auto module: modules) {
        if(auto* inputs = build_inputs(module)) {
            parts.push_back(inputs);
        }
    }
    DepsSnapshot inputs;
    merge_deps(inputs, parts);
    return inputs;
}

std::unordered_map<std::string, std::string>
    Project::imported_pcms(llvm::ArrayRef<Fid> modules) const {
    std::unordered_map<std::string, std::string> pcms;
    for(auto module: module_closure(*this, modules)) {
        auto it = pcm_builds.find(module);
        auto name = dep_graph.module_of(module);
        if(it != pcm_builds.end() && !it->second.path.empty() && !name.empty()) {
            pcms[name.str()] = it->second.path;
        }
    }
    return pcms;
}

bool Project::pcm_tracked(Fid path_id) const {
    return graph.has_node(module_node(path_id));
}

llvm::SmallVector<Fid> Project::invalidate_module(Fid path_id) {
    llvm::SmallVector<Fid> dirtied;
    for(auto id: graph.invalidate(module_node(path_id))) {
        dirtied.push_back(id.file());
    }
    return dirtied;
}

void Project::forget_buffer_scan(Fid path_id) {
    buffer_scans.erase(path_id);
}

}  // namespace clice

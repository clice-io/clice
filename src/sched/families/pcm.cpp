module;

#include "modules/prelude.h"

#include "support/anomaly.macros.h"
#include "support/logging.macros.h"

module clice;

import :sched.families.build_common;
import :sched.families.pcm;
import :support.anomaly;
import :support.logging;
import :syntax.header_context;
import :syntax.preamble;
import :syntax.scan;
import :vfs.file_system;
import :vfs.path;
import :worker.protocol;

namespace clice {

/// Changes whenever a PCM build of the same command comes to write another
/// BMI, or its key comes to name another build, so a blob or a record an
/// older clice stored is not taken for the new one.
constexpr llvm::StringRef pcm_version = "4";

/// The provider an import in `importer` resolves to among several of one
/// name: the nearest — the fewest directories up from the importer to one
/// holding it — and the first by path among equally near ones.
static Fid nearest_provider(const FileTable& files, llvm::ArrayRef<Fid> providers, Fid importer) {
    if(providers.size() == 1) {
        return providers.front();
    }
    auto distance = [&](Fid provider) {
        std::uint32_t up = 0;
        path::walk_ancestors(files.resolve(importer).parent(), [&](CanonicalRef dir) {
            up += 1;
            return !path::under(files.resolve(provider), dir);
        });
        return up;
    };
    return *std::ranges::min_element(providers, {}, [&](Fid provider) {
        return std::pair(distance(provider), llvm::StringRef(files.resolve(provider)));
    });
}

PCMFamily::PCMFamily(TaskGraph& graph,
                     Project& project,
                     CommandResolver& commands,
                     WorkerPool& pool) :
    graph(graph), project(project), commands(commands), pool(pool) {}

void PCMFamily::register_runner() {
    graph.register_family(Family::PCM, [this](RoundContext& ctx, NodeId id) {
        return run(ctx, Fid{static_cast<std::uint32_t>(id.key)});
    });
}

kota::task<PCMFamily::ModuleDeps> PCMFamily::direct_deps(Fid path_id) {
    // The same resolution the real build uses (run() below): a module unit
    // scanned with a different command than it compiles with would edge
    // against a different dependency set.
    std::string directory;
    std::vector<std::string> arguments;
    auto resolution = commands.resolve_command(path_id, directory, arguments);

    std::vector<const char*> argv;
    argv.reserve(arguments.size());
    for(auto& arg: arguments) {
        argv.push_back(arg.c_str());
    }
    co_return co_await direct_deps(path_id, resolution, argv, directory, std::nullopt);
}

kota::task<PCMFamily::ModuleDeps> PCMFamily::direct_deps(Fid path_id,
                                                         const Resolution& resolution,
                                                         llvm::ArrayRef<const char*> arguments,
                                                         llvm::StringRef directory,
                                                         std::optional<llvm::StringRef> content) {
    auto& dep_graph = project.dep_graph;
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
    for(auto* arg: arguments) {
        joined.push_back('\0');
        joined.append(arg);
    }
    auto arguments_hash = llvm::xxh3_64bits(joined);
    auto epoch = project.context_epoch;

    auto& memos = content ? buffer_scans : disk_scans;
    ScanMemo scan;
    if(auto memo = memos.find(path_id);
       memo != memos.end() && memo->second.directives == directives &&
       memo->second.bound == bound && memo->second.arguments == arguments_hash &&
       memo->second.epoch == epoch) {
        scan = memo->second;
    } else {
        llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> vfs;
        if(auto& synthesized = resolution.synthesized) {
            auto memory = llvm::makeIntrusiveRefCnt<llvm::vfs::InMemoryFileSystem>();
            for(auto& file: synthesized->files) {
                memory->addFile(file.path,
                                0,
                                llvm::MemoryBuffer::getMemBufferCopy(file.content, file.path));
            }
            auto overlay = llvm::makeIntrusiveRefCnt<llvm::vfs::OverlayFileSystem>(
                llvm::makeIntrusiveRefCnt<vfs::View>());
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
        auto scanned = co_await kota::queue(
            [&] { return scan_precise(arguments, directory, content, std::move(vfs)); });
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
        memos[path_id] = scan;
    }

    // Every scanned name goes into the edge set, resolved or not: an
    // unresolved name edges to its sentinel, which is what lets the
    // name's first provider re-dirty this unit through the ordinary
    // cascade later — no side bookkeeping of who failed against it.
    ModuleDeps deps{
        .offsets = {scan.offsets.begin(), scan.offsets.end()},
        .module_unit = scan.module_unit
    };
    for(auto& name: scan.imports) {
        auto providers = dep_graph.lookup_module(name);
        if(providers.empty()) {
            deps.declared.push_back(unresolved_node(name));
        } else {
            auto provider = nearest_provider(project.file_table, providers, path_id);
            deps.resolved.push_back(provider);
            deps.declared.push_back(node(provider));
        }
    }
    co_return deps;
}

llvm::SmallVector<NodeId> PCMFamily::provider_appeared(llvm::StringRef name) {
    return graph.update(unresolved_node(name));
}

void PCMFamily::declare_deps(Fid path_id, llvm::ArrayRef<NodeId> deps) {
    graph.declare(node(path_id), deps);
}

void PCMFamily::retire_blob(PCMBuild& build) {
    if(build.blob.empty()) {
        return;
    }
    if(!build.superseded.empty()) {
        project.store->invalidate("pcm", build.superseded);
    }
    build.superseded = std::exchange(build.blob, {});
}

kota::task<RoundOutcome> PCMFamily::run(RoundContext& ctx, Fid path_id) {
    // Inputs that moved may import otherwise: a scan taken before is no
    // answer for them, even while no event has reached the disk state yet.
    if(auto it = project.pcm_builds.find(path_id);
       it != project.pcm_builds.end() && deps_changed(project.file_table, it->second.deps)) {
        disk_scans.erase(path_id);
    }

    // The import list is scanner truth, not build output: commit it as
    // durable edges before building, so a unit whose build fails stays
    // cascade-reachable from its imports — fixing an import must re-dirty
    // the units it broke. The lazy per-round resolve is what keeps the
    // edges honest: a CDB or import change is always seen by the next
    // round. Each depend() then records the candidate edge (interest- and
    // foreground-visible immediately) and waits for the dependency.
    auto deps = co_await direct_deps(path_id);
    declare_deps(path_id, deps.declared);
    for(auto dep: deps.declared) {
        if(is_unresolved(dep)) {
            ctx.reference(dep);
        }
    }
    // The last round's failed import stands while this one waits: an
    // importer explains its diagnostics from it meanwhile. An import ready
    // early can have lost its PCM by the time the last one is, evicted or
    // being rebuilt: the key and the build read what each import built.
    for(bool built = false; !built;) {
        for(auto dep: deps.resolved) {
            switch(co_await ctx.depend(node(dep))) {
                case DependResult::Ready: break;
                case DependResult::Failed: {
                    auto& build = project.pcm_builds[path_id];
                    retire_blob(build);
                    build = {
                        .superseded = std::move(build.superseded),
                        .failure = PCMBuild::Failure{.import = dep},
                    };
                    co_return RoundOutcome::Failed;
                }
                case DependResult::Cancelled: co_return RoundOutcome::Stale;
            }
        }
        built = true;
        for(auto dep: deps.resolved) {
            auto& build = project.pcm_builds.find(dep)->second;
            if(build.path.empty() || drop_evicted(dep, build)) {
                built = false;
            }
        }
    }

    // Copied before any suspension below: while a PCM build is awaited, a
    // concurrent didSave can re-declare the file and drop the graph's
    // string.
    std::string module_name(project.dep_graph.module_of(path_id));
    if(module_name.empty())
        co_return RoundOutcome::Failed;

    auto file_path = std::string(project.file_table.resolve(path_id));

    worker::BuildPCMParams bp;
    bp.file = file_path;
    commands.resolve_command(path_id, bp.directory, bp.arguments);

    if(!project.store) {
        LOG_WARN("BuildPCM skipped for module {}: cache store is unavailable", module_name);
        co_return RoundOutcome::Failed;
    }

    // The key names everything of the build but file content, which the
    // inputs cover: the source path, the frontend-relevant subset of the
    // compile flags, the build configuration whose library records the
    // blob's dependency stamps, and what each import resolved to — the
    // keys of the imported modules, which name theirs in turn.
    std::string imports;
    for(auto dep: deps.declared) {
        if(is_unresolved(dep)) {
            imports += std::format("?{:x}", dep.key);
        } else {
            imports +=
                project.pcm_builds.find(Fid{static_cast<std::uint32_t>(dep.key)})->second.key;
        }
        imports.push_back('\0');
    }
    auto safe_module_name = module_name;
    std::ranges::replace(safe_module_name, ':', '-');
    auto pcm_key = std::format("{}-{}",
                               safe_module_name,
                               cache_key({clang::getClangFullVersion(),
                                          pcm_version,
                                          project.build.active_configuration(),
                                          bp.directory,
                                          file_path,
                                          canonicalize(bp.arguments),
                                          imports}));

    // The last build stands while its key and inputs do, failed or not: a
    // failed build rebuilt on unchanged inputs would make every compile
    // and completion of every importer pay for it first.
    llvm::StringRef pcm_miss = "no_entry";
    if(auto it = project.pcm_builds.find(path_id); it != project.pcm_builds.end()) {
        auto& build = it->second;
        if(build.key != pcm_key) {
            pcm_miss = "key_changed";
        } else if(deps_changed(project.file_table, build.deps)) {
            pcm_miss = "deps_changed";
        } else if(build.failure) {
            LOG_DEBUG("PCM build for module {} skipped: it failed on these inputs", module_name);
            co_return RoundOutcome::Failed;
        } else if(build.path.empty() || !project.store->lookup("pcm", build.blob)) {
            pcm_miss = "evicted";
        } else {
            LOG_PERF("cache", "ns=pcm event=hit key={} module={}", pcm_key, module_name);
            co_return RoundOutcome::Success;
        }
        // The build no longer describes the module: an importer compiling
        // while this build runs, or after it fails, must not read the
        // previous interface from it. Its PCM stays in the store until the
        // next one replaces it — a compile may still be reading it.
        build = {.blob = std::move(build.blob), .superseded = std::move(build.superseded)};
    }
    LOG_PERF("cache",
             "ns=pcm event=miss reason={} key={} module={}",
             pcm_miss,
             pcm_key,
             module_name);

    // Same shared-artifact refusal as the PCH, but keyed with the
    // module's current content: unlike pch_key (which embeds the
    // preamble text), pcm_key is content-free, and the refusal must lift
    // the moment the poison is edited.
    if(crashed(path_id)) {
        LOG_WARN("PCM build for module {} refused: it crashed a worker", module_name);
        co_return RoundOutcome::Failed;
    }

    bp.module_name = module_name;
    std::random_device random;
    auto blob = std::format("{}-{:08x}{:08x}", pcm_key, random(), random());
    auto pending = project.store->begin_store("pcm", blob);
    bp.output_path = pending.tmp_path;
    project.fill_pcm_deps(bp.pcms, closure(deps.resolved).getArrayRef());

    // The modules it imports are inputs as much as its own text — the
    // PCM embeds what it read of them — and theirs already carry their own
    // imports', so the snapshot is transitive. Taken before the build: an
    // eviction arriving during it must not drop them.
    auto imported = project.module_inputs(deps.resolved);

    // The interest class is read at dispatch time: a foreground requester
    // may have joined after this round started. The advisory token rides
    // into the pool, which cancels the request over IPC while this
    // frame keeps awaiting the real reply (contract 2 — the slot frees
    // only when the worker is truly idle).
    auto priority = ctx.foreground() ? worker::Priority::High : worker::Priority::Low;
    // Sampled before the build reads it: a save arriving mid-build is not
    // what crashed.
    auto content = content_hash(path_id);
    auto result = co_await deliver(
        pool,
        false,
        [&] { return pool.send_stateless(bp, priority, ctx.token()); },
        [&](const kota::ipc::Error& error) {
            build_crashes.insert_or_assign(path_id, Crash{content, error});
        });

    // A foreground reclaim or an advisory cancel is no decision on the
    // unit: report the round stale so waiters drive a retry instead of
    // failing their whole chain.
    if(!result.has_value() && result.error().code == worker::dispatch_errc::cancelled) {
        LOG_INFO("BuildPCM preempted for module {}, will retry", module_name);
        co_return RoundOutcome::Stale;
    }
    auto inputs = [&] {
        auto snapshot =
            capture_deps_snapshot(project.file_table, result.value().deps, result.value().build_at);
        merge_deps(snapshot, {&imported});
        return snapshot;
    };
    if(!result.has_value() || !result.value().success) {
        auto& build = project.pcm_builds[path_id];
        retire_blob(build);
        if(expected_build_failure(result)) {
            LOG_WARN("BuildPCM failed for module {}: {}",
                     module_name,
                     build_failure_message(result));
            // A round overtaken in flight records its inputs all the same:
            // they no longer match, and its successor rebuilds.
            if(result.has_value()) {
                build.key = pcm_key;
                build.deps = inputs();
                build.failure = PCMBuild::Failure{.error = result.value().first_error};
            }
        } else {
            LOG_ANOMALY(PCMBuildFail,
                        "PCM build failed for module {}: {}",
                        module_name,
                        build_failure_message(result));
        }
        co_return RoundOutcome::Failed;
    }

    // Commit on the thread pool: it fsyncs the freshly written PCM.
    auto committed =
        co_await kota::queue([&] { return project.store->commit(std::move(pending)); });
    auto& build = project.pcm_builds[path_id];
    retire_blob(build);
    if(!committed.has_value()) {
        LOG_WARN("Failed to commit PCM for module {}", module_name);
        co_return RoundOutcome::Failed;
    }
    build.key = std::move(pcm_key);
    build.blob = std::move(blob);
    build.path = std::move(committed.value());
    build.deps = inputs();
    LOG_INFO("Built PCM for module {}: {}", module_name, build.path);

    project.mark_artifacts_dirty();

    // Signal that new index data is available for background merge.
    if(on_indexing_needed)
        on_indexing_needed();

    co_return RoundOutcome::Success;
}

std::optional<PCMFamily::BuildFailure> PCMFamily::build_failure(Fid module) const {
    BuildFailure failure;
    for(auto current = module;;) {
        if(auto seen = std::ranges::find(failure.chain, current); seen != failure.chain.end()) {
            failure.cycle = seen - failure.chain.begin();
            return failure;
        }
        failure.chain.push_back(current);
        auto it = project.pcm_builds.find(current);
        if(it == project.pcm_builds.end() || !it->second.failure) {
            return std::nullopt;
        }
        if(!it->second.failure->import.valid()) {
            failure.error = it->second.failure->error;
            return failure;
        }
        current = it->second.failure->import;
    }
}

bool PCMFamily::forget_failure(Fid module) {
    auto it = project.pcm_builds.find(module);
    if(it == project.pcm_builds.end() || !it->second.failure) {
        return false;
    }
    it->second.failure.reset();
    return true;
}

llvm::SetVector<Fid> PCMFamily::closure(llvm::ArrayRef<Fid> modules) const {
    llvm::SetVector<Fid> read(modules.begin(), modules.end());
    for(std::size_t i = 0; i < read.size(); i += 1) {
        auto it = project.pcm_builds.find(read[i]);
        if(it == project.pcm_builds.end() || it->second.path.empty()) {
            continue;
        }
        for(auto dep: graph.dependencies(node(read[i]))) {
            if(dep.family == Family::PCM && !is_unresolved(dep)) {
                read.insert(Fid{static_cast<std::uint32_t>(dep.key)});
            }
        }
    }
    return read;
}

std::uint64_t PCMFamily::content_hash(Fid module) {
    auto seen = project.file_table.disk.current(module);
    return seen ? seen->hash : 0;
}

const kota::ipc::Error* PCMFamily::crashed(Fid module) {
    auto it = build_crashes.find(module);
    if(it == build_crashes.end()) {
        return nullptr;
    }
    // FIXME: only the module's own text lifts the refusal: a fix in a header it
    // includes waits for an importer's edit or save, and its importers fail to
    // index until then.
    if(content_hash(module) != it->second.content) {
        build_crashes.erase(it);
        return nullptr;
    }
    return &it->second.error;
}

bool PCMFamily::drop_evicted(Fid module, PCMBuild& build) {
    if(build.path.empty() || vfs::exists(build.path)) {
        return false;
    }
    // Artifact tier only: the blob vanished, its content did not, so
    // importers' committed results stay valid. A full invalidate() here
    // would void the very consumer round that is revalidating before
    // rebuilding its imports — under a cache budget smaller than the
    // working set, that voids and respawns the waiter forever.
    build.path.clear();
    build.blob.clear();
    graph.mark_dirty(node(module));
    return true;
}

bool PCMFamily::revalidate_blobs() {
    bool evicted = false;
    for(auto& [pid, build]: project.pcm_builds) {
        if(drop_evicted(pid, build)) {
            evicted = true;
        }
    }
    if(!evicted) {
        return false;
    }
    // The units importing an evicted one look again too, through every
    // level: their own PCMs still hold — their next rounds keep them — but
    // a round is what rebuilds an import first.
    for(bool marked = true; marked;) {
        marked = false;
        for(auto pid: std::views::transform(project.pcm_builds,
                                            [](auto& entry) -> auto& { return entry.first; })) {
            auto id = node(pid);
            if(!graph.has_node(id) || graph.is_dirty(id)) {
                continue;
            }
            if(std::ranges::any_of(graph.dependencies(id), [&](NodeId dep) {
                   return dep.family == Family::PCM && !is_unresolved(dep) && graph.is_dirty(dep);
               })) {
                graph.mark_dirty(id);
                marked = true;
            }
        }
    }
    return true;
}

kota::task<std::optional<PCMFamily::ModuleDeps>>
    PCMFamily::prepare_deps(Fid path_id,
                            const Resolution& resolution,
                            llvm::ArrayRef<const char*> arguments,
                            llvm::StringRef directory,
                            llvm::StringRef content) {
    // Resolved fresh on every call — a stale list must never outlive a
    // CDB change. The requester never runs a round here, but its
    // consumer edges must live in the graph: a saved module (or a
    // provider appearing for a sentinel) cascades to the open TUs
    // importing it through them. Declared even when empty, so a removed
    // import stops cascading.
    auto deps = co_await direct_deps(path_id,
                                     resolution,
                                     arguments,
                                     directory,
                                     std::optional<llvm::StringRef>(content));
    // A module unit's PCM node carries its ARTIFACT's edge truth, owned
    // by its own rounds — a request's buffer view must not overwrite it
    // (an unsaved removed import would disconnect the cached PCM from
    // the very dependency whose save should invalidate it). Plain TUs'
    // consumer nodes never run rounds; the declaration is theirs alone.
    if(project.dep_graph.module_of(path_id).empty()) {
        declare_deps(path_id, deps.declared);
    }
    if(deps.resolved.empty()) {
        co_return deps;
    }

    for(int attempt = 0; attempt < 3; attempt += 1) {
        bool any_evicted = revalidate_blobs();
        if(attempt > 0 && !any_evicted) {
            break;
        }

        // A user request waits on these builds: foreground, so the
        // background budget cannot throttle them. A module that did not
        // build is the compile's to report.
        std::vector<kota::task<JoinOutcome>> waits;
        waits.reserve(deps.resolved.size());
        for(auto dep: deps.resolved) {
            waits.push_back(graph.request(node(dep), {.foreground = true}));
        }
        auto results = co_await kota::when_all(std::move(waits));
        if(std::ranges::contains(results, JoinOutcome::Shutdown)) {
            co_return std::nullopt;
        }
    }
    co_return deps;
}

bool PCMFamily::tracks(Fid path_id) const {
    return graph.has_node(node(path_id));
}

llvm::SmallVector<Fid> PCMFamily::invalidate(Fid path_id) {
    llvm::SmallVector<Fid> dirtied;
    for(auto id: graph.update(node(path_id))) {
        dirtied.push_back(Fid{static_cast<std::uint32_t>(id.key)});
    }
    return dirtied;
}

}  // namespace clice

module clice;

import :index.format;
import :index.tu_index;
import :project.command;
import :project.graph.pch;
import :project.graph.pcm;
import :project.graph.precompiled;
import :project.graph.task_graph;
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
constexpr llvm::StringRef pcm_version = "7";

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

/// The entry's PCM, or the PCH it was built over, left the disk, or its
/// index proved unreadable: forget it, so the module's next attempt builds
/// again. Returns whether it had.
bool drop_evicted(PCMEntry& entry) {
    if(entry.path.empty()) {
        return false;
    }
    if(!fs::exists(entry.path)) {
        entry.blob.clear();
    } else if(!entry.index_path.empty() && (entry.pch.empty() || fs::exists(entry.pch))) {
        return false;
    }
    entry.path.clear();
    entry.index.reset();
    return true;
}

/// The failed builds from a module through the imports that failed it in
/// turn: to the module whose own build failed, or around a cycle of imports
/// failing each other.
struct FailedChain {
    llvm::SmallVector<Fid, 4> modules;

    /// The entry of the chain's last module: its own build's failure, or
    /// none when it did not fail; null for a module never built.
    const PCMEntry* last = nullptr;

    /// Where the import closing a cycle points back into `modules`.
    std::optional<std::size_t> cycle;
};

FailedChain failed_chain(const Project& project, Fid module) {
    FailedChain chain;
    for(auto current = module;;) {
        if(auto seen = ranges::find(chain.modules, current); seen != chain.modules.end()) {
            chain.cycle = seen - chain.modules.begin();
            return chain;
        }
        chain.modules.push_back(current);
        auto it = project.pcms.find(current);
        chain.last = it != project.pcms.end() ? &it->second : nullptr;
        if(!chain.last || !chain.last->failure || !chain.last->failure->result.import.valid()) {
            return chain;
        }
        current = chain.last->failure->result.import;
    }
}

/// The modules a compile importing `modules` reads, once it waited on them:
/// they and, below each that built, the modules it imports in turn — clang
/// loads every module below an import, and a built module's imports are
/// built too.
llvm::SetVector<Fid> module_closure(const Project& project, llvm::ArrayRef<Fid> modules) {
    llvm::SetVector<Fid> read(modules.begin(), modules.end());
    for(std::size_t i = 0; i < read.size(); i += 1) {
        if(!project.built_pcm(read[i])) {
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
/// can: built under the key, what it read unchanged, its PCM and its index
/// in the store and on disk.
/// A build that cannot no longer describes the module: an importer
/// compiling while the next one runs, or after it fails, must not read the
/// previous interface from it. Its PCM stays in the store until the next
/// one replaces it — a compile may still be reading it.
llvm::StringRef revalidate(Project& project, Fid module, llvm::StringRef pcm_key) {
    auto it = project.pcms.find(module);
    if(it == project.pcms.end()) {
        return "no_entry";
    }
    auto& entry = it->second;
    llvm::StringRef miss;
    if(entry.key != pcm_key) {
        miss = "key_changed";
    } else if(!inputs_hold(project.file_table, entry.inputs)) {
        miss = "inputs_changed";
    } else if(entry.path.empty() || !project.store->lookup("pcm", entry.blob) ||
              !project.store->lookup_aux("pcm", entry.blob) || !fs::exists(entry.path)) {
        miss = "evicted";
    } else if(entry.index_path.empty()) {
        miss = "index_unreadable";
    } else {
        return {};
    }
    entry.retire(&*project.store, "pcm");
    entry.key.clear();
    entry.pch.clear();
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
    auto& entry = project.pcms[module];
    auto* failed = entry.standing_failure(project.file_table);
    if(!failed) {
        return false;
    }
    if(failed->record.crashed()) {
        LOG_WARN("PCM build for module {} refused: it crashed a worker", name);
        return true;
    }
    if(failed->key == pcm_key) {
        LOG_DEBUG("PCM build for module {} skipped: it failed on these inputs", name);
        return true;
    }
    entry.failure.reset();
    return false;
}

/// One attempt of a module's PCM: declare the module's imports as its
/// edges, wait on them, keep the last build while it holds, else dispatch a
/// new one.
async::task<Result> build_module(Project& project, Attempt& attempt, Fid path_id) {
    // Inputs that moved may import otherwise: a scan taken before is no
    // answer for them, even while no event has reached the disk state yet.
    if(auto* inputs = project.module_inputs(path_id);
       inputs && !inputs_hold(project.file_table, *inputs)) {
        project.import_scans.disk.erase(path_id);
    }

    // The import list is scanner truth, not build output: commit it as
    // the node's edges before building, so a build that fails stays
    // reachable from its imports — fixing or providing one re-dirties it.
    // The scan runs under the command the build compiles with, resolved on
    // every attempt: a CDB or import change is always seen by the next one.
    worker::BuildPCMParams bp;
    auto resolution = project.resolve_command(path_id);
    bp.directory = resolution.directory;
    bp.project_root = project.root.str();
    bp.arguments = resolution.arguments;
    bp.hidden = resolution.hidden;
    auto imports = co_await project.scan_imports(path_id, resolution, std::nullopt);
    project.graph.set_edges(attempt.node(), imports.declared);

    // A unit of a target with precompiled headers builds over clice's own
    // build of them, as its command builds over the target's: the header
    // it forces in then reads nothing again, where parsed as text its
    // declarations would precede the module declaration. The PCM names
    // that PCH to its importers, which read it with the PCM: it is the
    // index's, keeping the function bodies, for every importer to read the
    // same one. One that did not build leaves the header to the unit's own
    // parse, as for any other unit.
    std::optional<std::string> pch_key;
    if(auto unit = project.pch_writer(path_id, resolution)) {
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
    auto resolved = imports.resolved();
    Fid failed_import;
    switch(co_await project.wait_modules(&attempt, resolved, [&](Fid module) {
        if(!failed_import.valid()) {
            failed_import = module;
        }
    })) {
        case Result::Success: break;
        case Result::Stale: co_return Result::Stale;
        case Result::Failed: {
            if(!failed_import.valid()) {
                // A build now would fail on the missing PCM and stand refused
                // on inputs that never failed it; failing unrecorded, the next
                // wait tries again.
                LOG_WARN("PCM build for {} skipped: an import's PCM keeps leaving the cache",
                         project.file_table.resolve(path_id));
                co_return Result::Failed;
            }
            auto& entry = project.pcms[path_id];
            entry.retire(project.store ? &*project.store : nullptr, "pcm");
            entry.key.clear();
            entry.pch.clear();
            entry.failure = {{.import = failed_import}, {}};
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
    Inputs precompiled;
    if(auto* pch = pch_key ? project.built_pch(*pch_key) : nullptr) {
        bp.pch = pch->path;
        pch_blob = pch->blob;
        precompiled = pch->inputs;
    }

    // The key names everything of the build but file content, which the
    // inputs cover: the source path, the frontend-relevant subset of the
    // compile flags, the index database that records the blob's
    // dependency stamps, what each import resolved to — the keys of the
    // imported modules, which name theirs in turn — the very build of the
    // PCH it reads: rebuilt, that one has another name — and how it indexes
    // the unit.
    std::string import_keys;
    for(auto dep: imports.declared) {
        if(is_unresolved_import(dep)) {
            import_keys += std::format("?{:x}", dep.key);
        } else {
            import_keys += project.built_pcm(dep.file())->key;
        }
        import_keys.push_back('\0');
    }
    bp.standard_library_instantiations = project.settings.project.standard_library_instantiations;
    auto safe_module_name = module_name;
    ranges::replace(safe_module_name, ':', '-');
    auto pcm_key = std::format("{}-{}",
                               safe_module_name,
                               cache_key({clang::getClangFullVersion(),
                                          pcm_version,
                                          std::to_string(index::index_format_version),
                                          bp.standard_library_instantiations ? "std" : "",
                                          project.database_key.str(),
                                          bp.directory,
                                          file_path,
                                          canonicalize(bp.arguments),
                                          import_keys,
                                          pch_blob}));

    auto pcm_miss = revalidate(project, path_id, pcm_key);
    if(pcm_miss.empty()) {
        LOG_PERF("cache", "ns=pcm event=hit key={} module={}", pcm_key, module_name);
        // A record read back from the database names no PCH.
        project.pcms.find(path_id)->second.pch = std::move(bp.pch);
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
    auto pending_index = project.store->begin_store_aux("pcm", blob);
    bp.output_path = pending.tmp_path;
    bp.index_output_path = pending_index.tmp_path;
    bp.pcms = project.imported_pcms(resolved);

    // The modules it imports are inputs as much as its own text — the PCM
    // embeds what it read of them — and theirs already carry their own
    // imports', so the snapshot is transitive. Taken before the build, with
    // the PCH it reads: an eviction arriving during it must not drop them.
    auto before = project.module_inputs(resolved);
    merge_inputs(before, {&precompiled});
    // What a build that crashes is known to read, sampled before it runs: a
    // save arriving mid-build is not what crashed.
    auto known = before;
    if(auto own = current_input(project.file_table, path_id)) {
        known.push_back(*own);
    }
    auto built =
        co_await project.send_precompiled(attempt,
                                          bp,
                                          std::format("module {}", module_name),
                                          std::move(known),
                                          before,
                                          [&](FailureRecord record, Inputs inputs) {
                                              // An attempt overtaken in flight records its inputs
                                              // all the same: they no longer match, and its
                                              // successor rebuilds.
                                              project.pcms[path_id].failure = {
                                                  {.key = pcm_key, .record = std::move(record)},
                                                  std::move(inputs)
                                              };
                                          });
    if(!built) {
        if(built.error() == Result::Failed) {
            project.pcms[path_id].retire(&*project.store, "pcm");
        }
        co_return built.error();
    }

    // Commit the pair on the thread pool, as a PCH's (see build_pair): the
    // fsyncs, and the walk opening the index verifies.
    std::optional<std::string> pcm_path;
    std::optional<std::string> index_path;
    std::shared_ptr<index::TUIndex> loaded;
    co_await async::queue([&] {
        auto committed = project.store->commit(std::move(pending));
        if(!committed) {
            return;
        }
        pcm_path = std::move(*committed);
        if(auto index = project.store->commit(std::move(pending_index))) {
            index_path = std::move(*index);
            loaded = load_paired_index(*index_path);
        }
        if(!loaded) {
            project.store->invalidate("pcm", blob);
        }
    });
    auto& entry = project.pcms[path_id];
    if(!loaded) {
        entry.retire(&*project.store, "pcm");
        LOG_WARN("Failed to commit PCM for module {}", module_name);
        co_return Result::Failed;
    }
    entry.publish(&*project.store,
                  "pcm",
                  std::move(blob),
                  std::move(*pcm_path),
                  std::move(*index_path),
                  std::move(loaded),
                  project.read_inputs(*built, before));
    entry.key = std::move(pcm_key);
    entry.pch = std::move(bp.pch);
    LOG_INFO("PCM built for module {}: {}", module_name, entry.path);

    project.mark_precompiled_dirty();

    co_return Result::Success;
}

}  // namespace

const PCMEntry* Project::built_pcm(Fid module) const {
    auto it = pcms.find(module);
    return it != pcms.end() && !it->second.path.empty() ? &it->second : nullptr;
}

bool Project::imports_present(llvm::ArrayRef<Fid> modules) {
    bool present = true;
    for(auto module: module_closure(*this, modules)) {
        auto it = pcms.find(module);
        if(it == pcms.end() || drop_evicted(it->second) || it->second.path.empty()) {
            present = false;
        }
    }
    return present;
}

llvm::SmallVector<Fid> Imports::resolved() const {
    llvm::SmallVector<Fid> modules;
    for(auto dep: declared) {
        if(!is_unresolved_import(dep)) {
            modules.push_back(dep.file());
        }
    }
    return modules;
}

bool Imports::modular() const {
    return module_unit ||
           ranges::any_of(declared, [](NodeId dep) { return !is_unresolved_import(dep); });
}

async::task<Imports> Project::scan_imports(Fid path_id,
                                           const Resolution& resolution,
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
        co_return Imports{};
    }

    llvm::SmallString<1024> joined(resolution.directory);
    for(auto& arg: resolution.arguments) {
        joined.push_back('\0');
        joined.append(arg);
    }
    auto arguments_hash = llvm::xxh3_64bits(joined);
    auto epoch = disk_epoch;

    auto& scans = content ? import_scans.buffers : import_scans.disk;
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
        import_scans.runs += 1;
        std::vector<const char*> argv;
        argv.reserve(resolution.arguments.size());
        for(auto& arg: resolution.arguments) {
            argv.push_back(arg.c_str());
        }
        auto scanned = co_await async::queue(
            [&] { return scan_precise(argv, resolution.directory, content, std::move(vfs)); });
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
    Imports imports{
        .offsets = {scan.offsets.begin(), scan.offsets.end()},
        .module_unit = scan.module_unit
    };
    for(auto& name: scan.imports) {
        auto providers = dep_graph.lookup_module(name);
        imports.declared.push_back(
            providers.empty() ? unresolved_import(name)
                              : module_node(nearest_provider(file_table, providers, path_id)));
    }
    co_return imports;
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

std::optional<feature::ModuleFailure> Project::module_failure(Fid module) const {
    auto chain = failed_chain(*this, module);
    feature::ModuleFailure failure{.cycle = chain.cycle};
    if(!chain.cycle) {
        auto* last = chain.last;
        if(!last || !last->failure || last->failure->result.record.crashed()) {
            return std::nullopt;
        }
        failure.error = last->failure->result.record.cause;
    }
    for(auto unit: chain.modules) {
        failure.chain.push_back(dep_graph.module_of(unit).str());
    }
    return failure;
}

bool Project::forget_build_failure(Fid module) {
    auto it = pcms.find(module);
    return it != pcms.end() && std::exchange(it->second.failure, std::nullopt).has_value();
}

const FailureRecord* Project::build_crash(Fid module) {
    // FIXME: what a crash is known to read leaves out the headers the module
    // includes: a fix there waits for an importer's edit or save, and its
    // importers fail to index until then.
    auto it = pcms.find(module);
    auto* failed = it != pcms.end() ? it->second.standing_failure(file_table) : nullptr;
    return failed && failed->record.crashed() ? &failed->record : nullptr;
}

async::task<std::optional<BuiltImports>> Project::build_imports(Fid file,
                                                                llvm::StringRef text,
                                                                const Resolution& resolution,
                                                                BuildOptions options) {
    // Resolved fresh on every call, from the text the parse will consume
    // under the command it compiles with: a context choice or a header's
    // host cannot enable an import the scan missed, and a stale list never
    // outlives a CDB change. An unsaved `import m;` builds its PCM now; the
    // module names still resolve against the on-disk module map, a module
    // unit being somebody else's saved file. The scan runs even for an empty
    // buffer: the command's injected -include prefix can still import.
    BuiltImports built{
        .imports = co_await scan_imports(file, resolution, std::optional<llvm::StringRef>(text)),
    };
    graph.set_edges(ast_node(file), built.imports.declared);
    // Still an edge when refused: the declaration above keeps it, so a later
    // build of the module, by a parse that may retry it, re-dirties this one.
    llvm::SmallVector<Fid> waited;
    for(auto module: built.imports.resolved()) {
        if(auto* crash = build_crash(module)) {
            if(!options.retry_crashed) {
                built.refused.push_back(*crash);
                continue;
            }
            forget_build_failure(module);
        }
        waited.push_back(module);
    }
    auto waits = co_await wait_modules(options.attempt, waited, [&](Fid module) {
        LOG_INFO("Import {} of {} failed to build; the parse reports it",
                 file_table.resolve(module),
                 file_table.resolve(file));
        built.failed.push_back(module);
    });
    if(waits == Result::Stale) {
        co_return std::nullopt;
    }
    co_return built;
}

async::task<Result> Project::wait_modules(Attempt* attempt,
                                          llvm::ArrayRef<Fid> modules,
                                          llvm::function_ref<void(Fid)> failed) {
    for(int pass = 0; pass < import_passes; pass += 1) {
        std::vector<Result> results;
        if(attempt) {
            for(auto module: modules) {
                results.push_back(
                    co_await attempt->depend(module_node(module), module_work(module)));
                if(results.back() == Result::Stale) {
                    co_return Result::Stale;
                }
            }
        } else {
            std::vector<async::task<Result>> waits;
            for(auto module: modules) {
                waits.push_back(graph.join(module_node(module),
                                           module_work(module),
                                           {.foreground = true, .retry = [] { return true; }}));
            }
            auto joined = co_await async::when_all(std::move(waits));
            results.assign(joined.begin(), joined.end());
            // Waiting through stale attempts, only a shutdown ends one Stale.
            if(ranges::contains(results, Result::Stale)) {
                co_return Result::Stale;
            }
        }
        bool built = true;
        for(auto [module, result]: views::zip(modules, results)) {
            if(result == Result::Failed) {
                built = false;
                if(failed) {
                    failed(module);
                }
            }
        }
        if(!built) {
            co_return Result::Failed;
        }
        if(imports_present(modules)) {
            co_return Result::Success;
        }
    }
    co_return Result::Failed;
}

const Inputs* Project::module_inputs(Fid module) const {
    auto chain = failed_chain(*this, module);
    if(chain.cycle || !chain.last) {
        return nullptr;
    }
    return chain.last->failure ? &chain.last->failure->inputs : &chain.last->inputs;
}

Inputs Project::module_inputs(llvm::ArrayRef<Fid> modules) const {
    llvm::SmallVector<const Inputs*> parts;
    for(auto module: modules) {
        if(auto* inputs = module_inputs(module)) {
            parts.push_back(inputs);
        }
    }
    Inputs inputs;
    merge_inputs(inputs, parts);
    return inputs;
}

std::unordered_map<std::string, std::string>
    Project::imported_pcms(llvm::ArrayRef<Fid> modules) const {
    std::unordered_map<std::string, std::string> paths;
    for(auto module: module_closure(*this, modules)) {
        auto* entry = built_pcm(module);
        auto name = dep_graph.module_of(module);
        if(entry && !name.empty()) {
            paths[name.str()] = entry->path;
        }
    }
    return paths;
}

std::shared_ptr<index::TUIndex> Project::module_index(Fid module) {
    auto it = pcms.find(module);
    if(it == pcms.end() || it->second.path.empty()) {
        return nullptr;
    }
    return it->second.load_index();
}

std::vector<std::shared_ptr<index::TUIndex>>
    Project::precompiled_indexes(llvm::StringRef pch_key, llvm::ArrayRef<Fid> modules) {
    auto indexes = pch_indexes(pch_key);
    if(!pch_key.empty() && indexes.empty()) {
        return {};
    }
    for(auto module: module_closure(*this, modules)) {
        if(!built_pcm(module) || dep_graph.module_of(module).empty()) {
            continue;
        }
        auto index = module_index(module);
        if(!index) {
            return {};
        }
        indexes.push_back(std::move(index));
    }
    return indexes;
}

llvm::SmallVector<Fid> Project::invalidate_module(Fid path_id) {
    llvm::SmallVector<Fid> dirtied;
    for(auto id: graph.invalidate(module_node(path_id))) {
        dirtied.push_back(id.file());
    }
    return dirtied;
}

}  // namespace clice

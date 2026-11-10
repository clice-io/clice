module clice;

import :command.database;
import :command.invocation;
import :project.command;
import :project.graph.pch;
import :project.graph.pcm;
import :project.graph.precompiled;
import :project.graph.task_graph;
import :project.graph.unit;
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
/// BMI or index its unit otherwise, or its key comes to name another build,
/// so a blob or a record an older clice stored is not taken for the new
/// one.
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

/// The entry's PCM, or the PCH it was built over, left the disk: forget it,
/// so the module's next attempt builds again. Returns whether it had.
bool drop_evicted(PCMEntry& entry) {
    if(entry.path.empty()) {
        return false;
    }
    if(!fs::exists(entry.path)) {
        entry.blob.clear();
    } else if(entry.pch.empty() || fs::exists(entry.pch)) {
        return false;
    }
    entry.path.clear();
    return true;
}

/// A PCM the build was handed, or the PCH it builds over, left the disk.
bool lost_import(const worker::BuildPCMParams& params) {
    return (!params.pch.empty() && !fs::exists(params.pch)) ||
           ranges::any_of(params.pcms, [](const auto& pcm) { return !fs::exists(pcm.second); });
}

/// The failed builds from a module through the imports that failed it in
/// turn: to the module whose own build failed, or around a cycle of imports
/// failing each other.
struct FailedChain {
    llvm::SmallVector<NodeId, 4> modules;

    /// The entry of the chain's last module: its own build's failure, or
    /// none when it did not fail; null for a module never built.
    const PCMEntry* last = nullptr;

    /// Where the import closing a cycle points back into `modules`.
    std::optional<std::size_t> cycle;
};

FailedChain failed_chain(const Project& project, NodeId module) {
    FailedChain chain;
    for(auto current = module;;) {
        if(auto seen = ranges::find(chain.modules, current); seen != chain.modules.end()) {
            chain.cycle = seen - chain.modules.begin();
            return chain;
        }
        chain.modules.push_back(current);
        auto it = project.pcms.find(current);
        chain.last = it != project.pcms.end() ? &it->second : nullptr;
        if(!chain.last || !chain.last->failure || !chain.last->failure->result.import) {
            return chain;
        }
        current = *chain.last->failure->result.import;
    }
}

/// The modules a compile importing `modules` reads, once it waited on them:
/// they and, below each that built, the modules it imports in turn — clang
/// loads every module below an import, and a built module's imports are
/// built too.
llvm::SetVector<NodeId> module_closure(const Project& project, llvm::ArrayRef<NodeId> modules) {
    llvm::SetVector<NodeId> read(modules.begin(), modules.end());
    for(std::size_t i = 0; i < read.size(); i += 1) {
        if(!project.built_pcm(read[i])) {
            continue;
        }
        for(auto module: imported_modules(project.graph, read[i])) {
            read.insert(module);
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
llvm::StringRef revalidate(Project& project, NodeId module, llvm::StringRef pcm_key) {
    auto it = project.pcms.find(module);
    if(it == project.pcms.end()) {
        return "none was built";
    }
    auto& entry = it->second;
    llvm::StringRef miss;
    if(entry.key != pcm_key) {
        miss = "its command, an import or its PCH changed";
    } else if(!inputs_hold(project.file_table, entry.inputs)) {
        miss = "a file it read changed";
    } else if(entry.path.empty() || !project.store->lookup("pcm", entry.blob) ||
              !fs::exists(entry.path)) {
        miss = "the last one left the cache";
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
bool refused(Project& project, NodeId module, llvm::StringRef pcm_key, llvm::StringRef name) {
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

/// The id of the context (ModuleContexts) of a module unit's entry whose
/// command `hash` names: nonzero, and clear of the bit unresolved_import
/// sets.
std::uint32_t context_id(std::uint64_t hash) {
    return static_cast<std::uint32_t>(hash >> 33) | 1;
}

/// The command a PCM node builds its unit under: the unit's own, or the
/// entry its context names; none when no entry does any more.
std::optional<Resolution> build_command(Project& project, NodeId module) {
    auto unit = module.file();
    auto context = module_context(module);
    if(context == 0) {
        return project.resolve_command(unit);
    }
    for(auto& candidate: project.candidates(unit)) {
        if(context_id(project.commands.command_hash(candidate.base)) == context) {
            ContextChoice choice{.base = project.commands.command_hash_hex(candidate.base)};
            return project.resolve_command(unit, {.choice = &choice});
        }
    }
    return std::nullopt;
}

/// module_compatibility of the arguments `command` renders.
ModuleCompatibility compatibility(Project& project, const Rendered& command) {
    llvm::SmallString<1024> joined(command.directory);
    for(auto& arg: command.arguments) {
        joined.push_back('\0');
        joined.append(arg);
    }
    auto [it, inserted] = project.compatibilities.try_emplace(llvm::xxh3_64bits(joined));
    if(!inserted) {
        return it->second;
    }
    std::vector<const char*> argv;
    argv.reserve(command.arguments.size());
    for(auto& arg: command.arguments) {
        argv.push_back(arg.c_str());
    }
    auto vfs = llvm::makeIntrusiveRefCnt<vfs::View>(command.hidden);
    clang::DiagnosticOptions options;
    auto diagnostics = clang::CompilerInstance::createDiagnostics(*vfs,
                                                                  options,
                                                                  new clang::IgnoringDiagConsumer(),
                                                                  true);
    if(auto invocation = create_compiler_invocation(argv, command.directory, vfs, diagnostics)) {
        it->second = module_compatibility(*invocation);
    }
    return it->second;
}

/// ModuleContexts::listed of a module unit, from its entries.
llvm::SmallVector<ModuleContexts::Context, 2> list_module_contexts(Project& project, Fid unit) {
    llvm::SmallVector<ModuleContexts::Context, 2> listed;
    llvm::SmallVector<std::uint64_t, 2> hashes;
    for(auto& entry: project.entries(unit)) {
        auto hash = project.commands.command_hash(entry.command);
        if(auto known = ranges::find(hashes, hash); known != hashes.end()) {
            auto& databases = listed[known - hashes.begin()].databases;
            if(!ranges::contains(databases, entry.source)) {
                databases.push_back(entry.source);
            }
            continue;
        }
        hashes.push_back(hash);
        listed.push_back(
            {.id = listed.empty() ? 0 : context_id(hash), .databases = {entry.source}});
    }
    if(listed.size() < 2) {
        return llvm::SmallVector<ModuleContexts::Context, 2>(1);
    }
    for(auto& context: listed) {
        auto command = build_command(project, module_node(unit, context.id));
        context.compatibility = compatibility(project, *command);
    }
    return listed;
}

/// The database `command`, what `file` compiles under, is an entry of; none
/// for a command no database lists.
std::optional<SourceID> database_of(Project& project, Fid file, const Candidate& command) {
    // The command is an entry of the first file it passed through, not of the
    // last: a header compiles in a provisional member under the command the
    // member borrowed.
    auto owner = command.rules_from.empty() ? file : command.rules_from.front();
    for(auto& entry: project.entries(owner)) {
        if(entry.command == command.base) {
            return entry.source;
        }
    }
    return std::nullopt;
}

/// One attempt of a module's PCM: declare the module's imports as its
/// edges, wait on them, keep the last build while it holds, else dispatch a
/// new one.
async::task<Result> build_module(Project& project, Attempt& attempt, NodeId node) {
    auto path_id = node.file();
    bool own_command = module_context(node) == 0;

    // The import list is scanner truth, not build output: commit it as
    // the node's edges before building, so a build that fails stays
    // reachable from its imports — fixing or providing one re-dirties it.
    // The scan runs under the command the build compiles with, resolved on
    // every attempt: a CDB or import change is always seen by the next one.
    // Under the unit's own command the build is the unit's compile, and
    // sends its index, recorded under the command as it is now — one
    // changing during the build leaves the unit owed.
    auto resolution = build_command(project, node);
    if(!resolution) {
        LOG_INFO("PCM build for {} skipped: the entry it was picked for is gone",
                 project.file_table.resolve(path_id));
        co_return Result::Failed;
    }
    worker::BuildPCMParams bp;
    auto command = project.command_identity(path_id);
    bp.directory = resolution->directory;
    bp.project_root = project.root.str();
    bp.arguments = resolution->arguments;
    bp.hidden = resolution->hidden;
    auto imports = co_await project.scan_imports(path_id, *resolution, std::nullopt);
    project.graph.set_edges(attempt.node(), imports.declared);

    // A unit of a target with precompiled headers builds over clice's own
    // build of them, as its command builds over the target's: the header
    // it forces in then reads nothing again, where parsed as text its
    // declarations would precede the module declaration. The PCM names
    // that PCH to its importers, which read it with the PCM. One that did
    // not build leaves the header to the unit's own parse, as for any
    // other unit.
    std::optional<std::string> pch_key;
    if(auto unit = project.pch_writer(path_id, *resolution)) {
        auto request = project.declared_pch_request(*unit);
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
    std::optional<NodeId> failed_import;
    switch(co_await project.wait_modules(&attempt, resolved, [&](NodeId module) {
        if(!failed_import) {
            failed_import = module;
        }
    })) {
        case Result::Success: break;
        case Result::Stale: co_return Result::Stale;
        case Result::Failed: {
            if(!failed_import) {
                // A build now would fail on the missing PCM and stand refused
                // on inputs that never failed it; failing unrecorded, the next
                // wait tries again.
                LOG_WARN("PCM build for {} skipped: an import's PCM keeps leaving the cache",
                         project.file_table.resolve(path_id));
                co_return Result::Failed;
            }
            auto& entry = project.pcms[node];
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
        LOG_INFO("PCM build for {} skipped: it declares no module",
                 project.file_table.resolve(path_id));
        co_return Result::Failed;
    }
    auto file_path = std::string(project.file_table.resolve(path_id));
    bp.file = file_path;

    if(!project.store) {
        auto& cache_dir = project.settings.project.cache_dir;
        auto cause = cache_dir.empty()
                         ? std::string("no cache directory to store its module file in")
                         : std::format("the cache directory {} cannot be used", cache_dir);
        project.pcms[node].failure = {{.record = {.cause = std::move(cause)}}, {}};
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
    // imported modules, which name theirs in turn — and the very build of
    // the PCH it reads: rebuilt, that one has another name.
    std::string import_keys;
    for(auto dep: imports.declared) {
        if(is_unresolved_import(dep)) {
            import_keys += std::format("?{:x}", dep.key);
        } else {
            import_keys += project.built_pcm(dep)->key;
        }
        import_keys.push_back('\0');
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
                                          import_keys,
                                          pch_blob}));

    auto why = revalidate(project, node, pcm_key);
    if(why.empty()) {
        LOG_PERF("cache", "ns=pcm event=hit key={} module={}", pcm_key, module_name);
        // A record read back from the database names no PCH.
        project.pcms.find(node)->second.pch = std::move(bp.pch);
        co_return Result::Success;
    }
    LOG_PERF("cache", "ns=pcm event=miss key={} module={}", pcm_key, module_name);

    if(refused(project, node, pcm_key, module_name)) {
        co_return Result::Failed;
    }

    bp.module_name = module_name;
    auto blob = new_blob(pcm_key);
    auto pending = project.store->begin_store("pcm", blob);
    bp.output_path = pending.tmp_path;
    bp.pcms = project.imported_pcms(resolved);
    IndexTransfer transfer;
    if(own_command) {
        bp.index = true;
        transfer = project.request_index(path_id, bp.indexing);
    }

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
    auto built = co_await project.send_precompiled(
        attempt,
        bp,
        std::format("module {} ({})", module_name, file_path),
        why,
        std::move(known),
        before,
        [&](FailureRecord record, Inputs inputs) {
            // A build whose imports left the disk under it failed on the
            // missing file: the next wait builds them again, and the module
            // with them.
            auto key = pcm_key;
            if(!record.crashed() && lost_import(bp)) {
                key.clear();
                record.cause = "an import's module file left the cache during the build";
            }
            // An attempt overtaken in flight records its inputs all the same:
            // they no longer match, and its successor rebuilds.
            project.pcms[node].failure = {
                {.key = std::move(key), .record = std::move(record)},
                std::move(inputs)
            };
        });
    if(!built) {
        if(built.error() == Result::Failed) {
            project.pcms[node].retire(&*project.store, "pcm");
        }
        co_return built.error();
    }
    if(own_command) {
        project.merge_build_index(path_id, transfer, *built, before, command);
    }

    // Commit on the thread pool: it fsyncs the freshly written PCM.
    auto committed =
        co_await async::queue([&] { return project.store->commit(std::move(pending)); });
    auto& entry = project.pcms[node];
    if(!committed.has_value()) {
        entry.retire(&*project.store, "pcm");
        LOG_WARN("Cannot store the PCM of module {}", module_name);
        co_return Result::Failed;
    }
    entry.publish(&*project.store,
                  "pcm",
                  std::move(blob),
                  std::move(committed.value()),
                  project.read_inputs(*built, before));
    entry.key = std::move(pcm_key);
    entry.pch = std::move(bp.pch);
    LOG_DEBUG("The PCM of module {} is stored at {}", module_name, entry.path);

    project.mark_precompiled_dirty();

    co_return Result::Success;
}

}  // namespace

const PCMEntry* Project::built_pcm(NodeId module) const {
    auto it = pcms.find(module);
    return it != pcms.end() && !it->second.path.empty() ? &it->second : nullptr;
}

bool Project::imports_present(llvm::ArrayRef<NodeId> modules) {
    bool present = true;
    for(auto module: module_closure(*this, modules)) {
        auto it = pcms.find(module);
        if(it == pcms.end() || drop_evicted(it->second) || it->second.path.empty()) {
            present = false;
        }
    }
    return present;
}

llvm::SmallVector<NodeId> Imports::resolved() const {
    llvm::SmallVector<NodeId> modules;
    for(auto dep: declared) {
        if(!is_unresolved_import(dep)) {
            modules.push_back(dep);
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

    // A buffer's own text is no input of its scan: the directives stand
    // for it.
    auto& scans = content ? buffer_scans : disk_scans;
    ImportScan scan;
    if(auto* memo = scans.lookup(file_table, path_id, content ? path_id : Fid{});
       memo && memo->result.directives == directives && memo->result.bound == bound &&
       memo->result.arguments == arguments_hash) {
        scan = memo->result;
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
        import_scan_runs += 1;
        std::vector<const char*> argv;
        argv.reserve(resolution.arguments.size());
        for(auto& arg: resolution.arguments) {
            argv.push_back(arg.c_str());
        }
        auto started_at = chrono::duration_cast<chrono::milliseconds>(
                              chrono::system_clock::now().time_since_epoch())
                              .count();
        auto scanned = co_await async::queue(
            [&] { return scan_precise(argv, resolution.directory, content, std::move(vfs)); });
        scan = {.directives = directives,
                .bound = bound,
                .arguments = arguments_hash,
                .imports = std::move(scanned.modules),
                .offsets = std::move(scanned.import_offsets),
                .module_unit = !scanned.module_name.empty()};
        // An implementation unit imports its own module implicitly.
        if(scanned.is_implementation_unit()) {
            scan.imports.push_back(std::move(scanned.module_name));
            scan.offsets.push_back(std::numeric_limits<std::uint32_t>::max());
        }
        // A header context's files exist in memory alone: the chain files
        // its text embeds stand for them.
        if(auto& synthesized = resolution.synthesized) {
            llvm::StringSet<> memory;
            for(auto& file: synthesized->files) {
                memory.insert(CanonicalPath(Spelling::absolute(file.path)).str());
            }
            std::erase_if(scanned.deps,
                          [&](const ReportedInput& input) { return memory.contains(input.path); });
        }
        auto inputs = intern_inputs(file_table, scanned.deps, started_at);
        inputs.append(resolution.inputs.begin(), resolution.inputs.end());
        scans.commit(path_id, scan, std::move(inputs));
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
                              : module_build(nearest_provider(file_table, providers, path_id),
                                             path_id,
                                             resolution));
    }
    co_return imports;
}

Work Project::module_work(NodeId module) {
    return {
        .run = [this, module](Attempt& attempt) { return build_module(*this, attempt, module); },
        .check = [this,
                  module] { return imports_present(module) ? Check::Holds : Check::ProductGone; },
    };
}

NodeId Project::module_build(Fid module, Fid importer, const Resolution& command) {
    auto& contexts = module_contexts[module];
    if(contexts.epoch != commands_epoch) {
        contexts.listed = list_module_contexts(*this, module);
        contexts.epoch = commands_epoch;
    }
    if(contexts.listed.size() < 2) {
        return module_node(module);
    }
    auto wanted = compatibility(*this, command);
    auto database = database_of(*this, importer, command.candidate);
    const ModuleContexts::Context* readable = nullptr;
    for(auto& context: contexts.listed) {
        if(!wanted.reads(context.compatibility)) {
            continue;
        }
        if(database && ranges::contains(context.databases, *database)) {
            readable = &context;
            break;
        }
        if(!readable) {
            readable = &context;
        }
    }
    if(!readable || readable->id == 0) {
        return module_node(module);
    }
    if(!ranges::contains(contexts.picked, readable->id)) {
        contexts.picked.push_back(readable->id);
    }
    return module_node(module, readable->id);
}

std::vector<feature::ModuleFailure> Project::module_failures(llvm::ArrayRef<NodeId> imports) const {
    std::vector<feature::ModuleFailure> failures;
    for(auto module: imports) {
        auto chain = failed_chain(*this, module);
        feature::ModuleFailure failure{.cycle = chain.cycle};
        if(!chain.cycle) {
            auto* last = chain.last;
            if(!last || !last->failure || last->failure->result.record.crashed()) {
                continue;
            }
            failure.error = last->failure->result.record.cause;
        }
        for(auto unit: chain.modules) {
            failure.chain.push_back(dep_graph.module_of(unit.file()).str());
        }
        failures.push_back(std::move(failure));
    }
    return failures;
}

bool Project::forget_module_failure(NodeId module) {
    auto it = pcms.find(module);
    return it != pcms.end() && std::exchange(it->second.failure, std::nullopt).has_value();
}

const FailureRecord* Project::module_build_crash(NodeId module) {
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
    llvm::SmallVector<NodeId> waited;
    for(auto module: built.imports.resolved()) {
        if(auto* crash = module_build_crash(module)) {
            if(!options.retry_crashed) {
                built.refused.push_back(*crash);
                continue;
            }
            forget_module_failure(module);
        }
        waited.push_back(module);
    }
    auto waits = co_await wait_modules(options.attempt, waited, [&](NodeId module) {
        built.failed.push_back(module);
    });
    if(waits == Result::Stale) {
        co_return std::nullopt;
    }
    if(!built.failed.empty()) {
        LOG_INFO("Imports of {} failed to build: {}; the parse reports them",
                 file_table.resolve(file),
                 llvm::join(built.failed | views::transform([&](NodeId module) {
                                return dep_graph.module_of(module.file());
                            }),
                            ", "));
    }
    co_return built;
}

async::task<Result> Project::wait_modules(Attempt* attempt,
                                          llvm::ArrayRef<NodeId> modules,
                                          llvm::function_ref<void(NodeId)> failed) {
    for(int pass = 0; pass < import_passes; pass += 1) {
        std::vector<Result> results;
        if(attempt) {
            for(auto module: modules) {
                results.push_back(co_await attempt->depend(module, module_work(module)));
                if(results.back() == Result::Stale) {
                    co_return Result::Stale;
                }
            }
        } else {
            std::vector<async::task<Result>> waits;
            for(auto module: modules) {
                waits.push_back(graph.join(module,
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

const Inputs* Project::module_inputs(NodeId module) const {
    auto chain = failed_chain(*this, module);
    if(chain.cycle || !chain.last) {
        return nullptr;
    }
    return chain.last->failure ? &chain.last->failure->inputs : &chain.last->inputs;
}

Inputs Project::module_inputs(llvm::ArrayRef<NodeId> modules) const {
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
    Project::imported_pcms(llvm::ArrayRef<NodeId> modules) const {
    std::unordered_map<std::string, std::string> paths;
    for(auto module: module_closure(*this, modules)) {
        auto* entry = built_pcm(module);
        auto name = dep_graph.module_of(module.file());
        if(entry && !name.empty()) {
            paths[name.str()] = entry->path;
        }
    }
    return paths;
}

llvm::SmallVector<NodeId> Project::module_nodes(Fid unit) const {
    llvm::SmallVector<NodeId> nodes{module_node(unit)};
    if(auto it = module_contexts.find(unit); it != module_contexts.end()) {
        for(auto context: it->second.picked) {
            nodes.push_back(module_node(unit, context));
        }
    }
    return nodes;
}

}  // namespace clice

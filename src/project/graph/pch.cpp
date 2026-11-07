module clice;

import :command.argument;
import :compile.compilation;
import :project.command;
import :project.graph.pch;
import :project.graph.pcm;
import :project.graph.precompiled;
import :project.graph.task_graph;
import :project.graph.unit;
import :project.project;
import :settings.settings;
import :support.logging;
import :support.text;
import :syntax.preamble;
import :vfs.cache_store;
import :vfs.file_table;
import :vfs.path;
import :worker.protocol;

namespace clice {

namespace {

/// Discard the pairs the key's entry names, and forget the entry unless it
/// holds what failed of the key: an entry whose pairs the store's LRU
/// evicted, or whose PCH left the disk behind the store's back, goes, or
/// the content-keyed map grows for the server's lifetime.
void forget_pairs(Project& project, llvm::StringRef pch_key) {
    auto it = project.pchs.find(pch_key);
    if(it == project.pchs.end()) {
        return;
    }
    auto& entry = it->second;
    entry.discard(project.store ? &*project.store : nullptr, "pch");
    entry.index_path.clear();
    entry.index.reset();
    entry.built_index.reset();
    if(!entry.failure && !entry.consumes) {
        project.pchs.erase(it);
    }
}

/// Whether the PCH registered under the key can be adopted as it is, and
/// why not: complete, in the store, on disk, a preamble's index readable —
/// else it is gone, and builds again from what it read — and what it read
/// unchanged, the build of the PCH it chains onto among it. A preamble's
/// pair needs both halves: a PCH whose pch.idx index is gone (crash between
/// commits, failed aux commit) rebuilds whole. The store lookup refreshes
/// the blob's LRU position, and a passing inputs check may repair the
/// snapshot's stat fast path in place.
std::pair<Check, llvm::StringRef> pair_state(Project& project, llvm::StringRef pch_key) {
    auto it = project.pchs.find(pch_key);
    if(it == project.pchs.end() || it->second.path.empty()) {
        return {Check::ProductGone, "no_entry"};
    }
    auto& entry = it->second;
    if(!project.store || !project.store->lookup("pch", entry.blob) ||
       (!entry.whole_file && !project.store->lookup_aux("pch", entry.blob)) ||
       !fs::exists(entry.path)) {
        return {Check::ProductGone, "evicted"};
    }
    // load_index() found the blob unreadable earlier; republish the pair
    // rather than serving a PCH with no index forever.
    if(!entry.whole_file && entry.index_path.empty()) {
        return {Check::ProductGone, "idx_unreadable"};
    }
    if(!entry.base.empty()) {
        auto* base = project.built_pch(entry.base);
        if(!base || base->blob != entry.base_blob) {
            return {Check::InputsChanged, "base_rebuilt"};
        }
    }
    if(!inputs_hold(project.file_table, entry.inputs)) {
        return {Check::InputsChanged, "inputs_changed"};
    }
    return {Check::Holds, {}};
}

/// Whether a failure stands against building the key again. A build that
/// failed stands while its inputs do: one that crashed a worker is refused
/// before the dispatch, since the PCH is shared and every document with
/// this preamble would burn a worker of its own, and one that failed on the
/// user's code would fail the same way, every compile of every consumer
/// paying for it first. A preamble's key is content-derived: editing the
/// poison starts a fresh key. A precompiled header the build declares is
/// keyed by its command alone: its crash stands until its own file changes,
/// its consumers parsing the header it holds meanwhile. A key whose pairs
/// its consumers keep failing to read stops the same way: rebuilding one
/// would fare no better (see pch_consume_failed).
bool refused(Project& project, llvm::StringRef pch_key, llvm::StringRef file) {
    auto it = project.pchs.find(pch_key);
    if(it == project.pchs.end()) {
        return false;
    }
    auto& entry = it->second;
    if(auto* failed = entry.standing_failure(project.file_table)) {
        if(failed->crashed()) {
            LOG_WARN("PCH build for {} refused: key {} crashed a worker", file, pch_key);
        } else {
            LOG_DEBUG("PCH build for {} skipped: key {} failed on these inputs", file, pch_key);
        }
        return true;
    }
    if(entry.consumes && entry.consumes->stops(pch_consume_policy, FailureRecord::Clock::now())) {
        LOG_WARN("PCH build for {} refused: its consumers keep failing to read key {}",
                 file,
                 pch_key);
        return true;
    }
    return false;
}

/// One attempt: revalidate the registered PCH, or dispatch a build with the
/// request's inputs and commit it, a preamble's with its index.
async::task<Result> build_pair(Project& project, Attempt& attempt, PCHRequest request) {
    const auto& pch_key = request.pch_key;

    auto [state, pch_miss] = pair_state(project, pch_key);
    if(state == Check::Holds) {
        LOG_PERF("cache", "ns=pch event=hit key={} file={}", pch_key, request.params.file);
        co_return Result::Success;
    }
    LOG_PERF("cache",
             "ns=pch event=miss reason={} key={} file={}",
             pch_miss,
             pch_key,
             request.params.file);
    if(pch_miss == "evicted") {
        forget_pairs(project, pch_key);
    }

    if(!project.store) {
        LOG_WARN("PCH build skipped: cache store is unavailable");
        co_return Result::Failed;
    }
    if(refused(project, pch_key, request.params.file)) {
        co_return Result::Failed;
    }

    // Build via a stateless worker: it writes the PCH, and a preamble's
    // pch.idx index, to the tmp paths allocated here; the store commits
    // (fsync + rename) both on success, primary first. A whole file's build
    // that is its writing unit's compile sends the unit's index instead.
    auto blob = new_blob(pch_key);
    auto pending = project.store->begin_store("pch", blob);
    auto& bp = request.params;
    bp.project_root = project.root.str();
    bp.output_path = pending.tmp_path;
    std::optional<CacheStore::PendingEntry> pending_idx;
    IndexTransfer transfer;
    if(!bp.whole_file) {
        pending_idx = project.store->begin_store_aux("pch", blob);
        bp.indexing.output_path = pending_idx->tmp_path;
    } else if(request.writer) {
        bp.index = true;
        transfer = project.request_index(request.writer->unit, bp.indexing);
    }

    // What the imported modules' PCMs and the PCH built over were built
    // from as the build starts: one rebuilt meanwhile stales the PCH.
    auto before = project.module_inputs(request.modules);
    std::string base_blob;
    if(!request.base.empty()) {
        auto* base = project.built_pch(request.base);
        if(!base) {
            LOG_DEBUG("PCH build for {} skipped: the PCH it builds over is gone", bp.file);
            co_return Result::Failed;
        }
        bp.pch = base->path;
        base_blob = base->blob;
        merge_inputs(before, {&base->inputs});
    }
    // What a build that crashes is known to read, sampled before it runs:
    // the modules and the PCH it builds over, and a whole file's own text,
    // which its key leaves out.
    auto known = before;
    if(bp.whole_file) {
        if(auto own = current_input(project.file_table,
                                    project.file_table.intern(Spelling::absolute(bp.file)))) {
            known.push_back(*own);
        }
    }

    LOG_DEBUG("Building PCH for {}, bound={}, key={}", bp.file, bp.preamble_bound, pch_key);

    // A crash is recorded on the key; its consumers book it on their
    // documents when they find it there. A background index unit waiting on
    // a precompiled header the build declares leaves the interactive work
    // its priority.
    auto built = co_await project.send_precompiled(
        attempt,
        bp,
        bp.file,
        std::move(known),
        before,
        [&](FailureRecord record, Inputs inputs) {
            project.pchs[pch_key].failure = {std::move(record), std::move(inputs)};
        });
    if(!built) {
        co_return built.error();
    }
    if(request.writer) {
        project.merge_build_index(request.writer->unit,
                                  transfer,
                                  *built,
                                  before,
                                  request.writer->command);
    }

    // Commit on the thread pool as one job: the fsyncs stay off the event
    // loop, and the store either publishes a preamble's whole pair or
    // discards it (a half pair would hand adopters a PCH whose pch.idx index
    // is gone). Opening the freshly committed blob (mmap + verification,
    // which walks every section's shard) also happens here so no later
    // consumer pays that walk on the event loop.
    std::optional<std::string> pch_path;
    std::optional<std::string> index_path;
    std::shared_ptr<index::TUIndex> loaded;
    co_await async::queue([&] {
        auto committed = project.store->commit(std::move(pending));
        if(!committed) {
            // pending_idx cleans its own tmp blob when the frame unwinds.
            return;
        }
        pch_path = std::move(*committed);
        if(!pending_idx) {
            return;
        }
        // The pair is only usable complete: when the index blob cannot be
        // published, discard the PCH too — the next attempt rebuilds both.
        if(auto index = project.store->commit(std::move(*pending_idx))) {
            index_path = std::move(*index);
            loaded = load_pch_index(*index_path);
        }
        // A committed pch.idx index that fails verification is as unusable as
        // an uncommitted one: discard the pair rather than publish a PCH
        // whose preamble index every consumer would fail to load.
        if(!loaded) {
            project.store->invalidate("pch", blob);
        }
    });
    if(!pch_path) {
        LOG_WARN("Failed to commit PCH for {}", bp.file);
        co_return Result::Failed;
    }
    if(pending_idx && !loaded) {
        if(!index_path) {
            LOG_WARN("Failed to commit pch.idx envelope for {}", bp.file);
        } else {
            LOG_WARN("Freshly committed pch.idx envelope for {} is unreadable", bp.file);
        }
        // The commit job discarded the new pair, and the previous one is
        // stale (that is why it was rebuilt): drop it rather than let it
        // revalidate against its old deps.
        forget_pairs(project, pch_key);
        co_return Result::Failed;
    }

    auto& entry = project.pchs[pch_key];
    entry.publish(&*project.store,
                  "pch",
                  std::move(blob),
                  std::move(*pch_path),
                  project.read_inputs(*built, before));
    entry.bound = request.params.preamble_bound;
    entry.whole_file = bp.whole_file;
    entry.base = request.base;
    entry.base_blob = std::move(base_blob);
    entry.index_path = index_path.value_or("");
    // Replace the previous blob's mapping (same key, rebuilt content);
    // in-flight holders of the old shared_ptr stay valid. Handed to a parse
    // still waiting for it: with none, nothing would ever take it.
    entry.index = loaded;
    if(!attempt.token().cancelled()) {
        entry.built_index = std::move(loaded);
    }

    LOG_INFO("PCH built for {}: {}", bp.file, entry.path);

    project.mark_precompiled_dirty();

    co_return Result::Success;
}

}  // namespace

std::shared_ptr<index::TUIndex> load_pch_index(llvm::StringRef path) {
    auto file = fs::File::open(path);
    if(!file) {
        return nullptr;
    }
    auto buffer = llvm::MemoryBuffer::getOpenFile(file->native_handle(),
                                                  path,
                                                  /*FileSize=*/-1,
                                                  /*RequiresNullTerminator=*/false,
                                                  /*IsVolatile=*/false);
    if(!buffer) {
        return nullptr;
    }
    // A stale or truncated pair must never crash the server: the TU index
    // is deep-verified, and every embedded shard blob once — queries then
    // run unchecked. Anything failing reads as "pair missing" and the PCH
    // is rebuilt.
    auto tu_index = index::TUIndex::from_buffer(std::move(*buffer));
    if(!tu_index.loaded() || !tu_index.verify_shards()) {
        return nullptr;
    }
    return std::make_shared<index::TUIndex>(std::move(tu_index));
}

std::shared_ptr<index::TUIndex> PCHEntry::load_index() {
    if(auto held = index.lock()) {
        return held;
    }
    if(index_path.empty()) {
        return nullptr;
    }
    auto loaded = load_pch_index(index_path);
    if(!loaded) {
        // Unreadable blob: clear the path so queries don't retry the
        // mmap + verification on every call. The pair now looks
        // incomplete and the next acquisition rebuilds it.
        LOG_WARN("Failed to open pch.idx envelope {}", index_path);
        index_path.clear();
    }
    index = loaded;
    return loaded;
}

PCHRequest Project::declared_pch_request(const CommandRef& unit) {
    // The key names the build but the file's content, which the build's
    // inputs cover, its own source among them.
    auto declared = [&](const CommandRef& command) {
        auto rendered = render(command);
        PCHRequest request{
            .params = {.file = std::string(file_table.resolve(command.file)),
                       .directory = std::move(rendered.directory),
                       .arguments = std::move(rendered.arguments),
                       .hidden = std::move(rendered.hidden),
                       .whole_file = true},
        };
        auto& params = request.params;
        request.pch_key = cache_key({clang::getClangFullVersion(),
                                     pch_build_rule(false, false),
                                     database_key.str(),
                                     params.directory,
                                     params.file,
                                     canonicalize(params.arguments)});
        return request;
    };
    auto request = declared(unit);
    // A consumer's rules may edit the writing unit's command otherwise than
    // the unit's own do: that build is no compile of the unit.
    if(declared(select_command(unit.file).ref).pch_key == request.pch_key) {
        request.writer = PCHRequest::Writer{unit.file, command_identity(unit.file)};
    }
    return request;
}

const PCHEntry* Project::built_pch(llvm::StringRef key) const {
    auto it = pchs.find(key);
    return it != pchs.end() && !it->second.path.empty() ? &it->second : nullptr;
}

PreamblePCH Project::preamble_pch(Fid path_id,
                                  llvm::StringRef text,
                                  std::optional<std::uint32_t> point,
                                  const Resolution& resolution,
                                  const Imports& imports,
                                  llvm::StringRef base,
                                  bool forced) {
    auto path = file_table.resolve(path_id);
    auto* synthesized = resolution.synthesized.get();
    // The part of the prefix inside braces comes after the preamble, which
    // the header's own directives would otherwise precede. A preamble still
    // being typed ends before its unfinished directive: the finished part
    // keeps its PCH — the key of the one before, when the directive goes at
    // the end of the block — and the unfinished one parses with the body,
    // which reports its own error. A global module fragment declaring
    // anything itself is not cut so: the module declaration would follow a
    // declaration of the body.
    auto bound = preamble_bound(text);
    bool partial = false;
    if(point) {
        auto before = preamble_bound(text, logical_line_begin(text, *point));
        partial = before < bound;
        bound = before;
    }
    auto finished = finished_preamble_bound(text, bound);
    bool cut_fragment = finished < bound && preamble_declares(text, bound);
    bound = (synthesized && !synthesized->open.empty()) || cut_fragment ? 0 : finished;
    if(bound == 0 && !synthesized && !forced) {
        // No preamble directives and no injected -include — PCH would be
        // empty. Self-contained header contexts end up here too: they borrow
        // a command but inject nothing. A precompiled header the build
        // declares is loaded by the parse itself.
        return {.partial = partial};
    }

    // With a synthesized prefix, the PCH is worth building even at
    // bound == 0: the -include'd prefix is processed via the predefines
    // buffer and ends up in the PCH, so the (potentially huge) prefix is not
    // re-parsed on every edit. The -include flag is part of the
    // canonicalized arguments below, and the prefix's name hashes its
    // content — through the fragments it includes, the whole chain's — so
    // the key tracks prefix changes automatically.

    // The modules imported by the preamble, or by a file the command line
    // includes, are inputs of the PCH: it depends on what their PCMs were
    // built from, and keys on which PCMs those are — a module that gains or
    // loses its PCM, or whose provider or command changes, starts a new
    // PCH. A PCH loses the imports of a module unit's global module
    // fragment: the module declaration past it takes the fragment for
    // absent and hides what the fragment made visible. Such a preamble
    // goes without one.
    std::string pcm_keys;
    llvm::SmallVector<NodeId> modules;
    bool imported = false;
    assert(ranges::size(imports.declared) == ranges::size(imports.offsets));
    for(auto [dep, offset]: views::zip(imports.declared, imports.offsets)) {
        if(offset != 0 && offset >= bound) {
            continue;
        }
        imported = true;
        if(!is_unresolved_import(dep)) {
            if(auto* pcm = built_pcm(dep)) {
                pcm_keys += pcm->key;
                modules.push_back(dep);
            }
        }
        pcm_keys.push_back('\0');
    }
    if(imported && imports.module_unit) {
        LOG_DEBUG("No PCH for {}: its global module fragment imports", path);
        return {.partial = partial};
    }

    // Key the PCH by preamble text plus the frontend-relevant compile flags,
    // so files with the same preamble text but different flags (-D, -I, -std)
    // produce separate PCHs.  The source file path stays out of the key so
    // files with identical preambles share one PCH — but its DIRECTORY (and
    // the working directory) must stay in: quote includes and relative paths
    // resolve against them, so equal preamble text in different directories
    // can mean different content.  A preamble defining or testing a macro,
    // or declaring in a global module fragment, keeps the path in too: the
    // PCH places the macro and its uses, or the declaration, in the file
    // that built it, where a file sharing it would find its own.  The
    // clang version guards against reusing
    // blobs a newer bundled clang would reject, and the database key keeps
    // the blob with the index database that records its dependencies:
    // shared across databases, one could rebuild it while another's
    // records still confirmed the old content. A PCH built over the
    // build's precompiled header names the very build it chains onto:
    // rebuilt, that one has another name.
    auto preamble_text = text.substr(0, bound);
    llvm::StringRef base_blob;
    if(auto it = pchs.find(base); it != pchs.end()) {
        base_blob = it->second.blob;
    }
    auto own_path = preamble_uses_macros(text, bound) || preamble_declares(text, bound)
                        ? llvm::StringRef(path)
                        : llvm::StringRef();
    bool skip_bodies = settings.project.skip_header_bodies;
    bool modular = imports.modular();
    auto pch_key = cache_key({clang::getClangFullVersion(),
                              pch_build_rule(skip_bodies, modular),
                              database_key.str(),
                              resolution.directory,
                              path::parent(path),
                              own_path,
                              preamble_text,
                              canonicalize(resolution.arguments),
                              pcm_keys,
                              base_blob});
    PreamblePCH plan{
        .request =
            PCHRequest{
                       .pch_key = std::move(pch_key),
                       .params =
                    {
                        .file = std::string(path),
                        .directory = resolution.directory,
                        .arguments = resolution.arguments,
                        .hidden = resolution.hidden,
                        .synthesized = synthesized ? synthesized->files : SynthesizedFiles{},
                        .content = std::string(text),
                        .preamble_bound = bound,
                        .skip_bodies = skip_bodies,
                        .modular = modular,
                    }, .base = base.str(),
                       },
        .partial = partial,
    };
    if(imported) {
        plan.request->params.pcms = imported_pcms(imports.resolved());
        plan.request->modules = std::move(modules);
    }
    return plan;
}

bool Project::pch_reads(llvm::StringRef key, Fid file) const {
    auto* entry = built_pch(key);
    return entry &&
           ranges::any_of(entry->inputs, [&](const Input& input) { return input.file == file; });
}

Work Project::pch_work(PCHRequest request) {
    auto shared = std::make_shared<const PCHRequest>(std::move(request));
    return {
        .run = [this, shared](Attempt& attempt) { return build_pair(*this, attempt, *shared); },
        .check = [this, shared] { return pair_state(*this, shared->pch_key).first; },
    };
}

async::task<std::optional<BuiltPCH>> Project::build_pch(Fid file,
                                                        llvm::StringRef text,
                                                        std::optional<std::uint32_t> point,
                                                        const Resolution& resolution,
                                                        const Imports& imports,
                                                        BuildOptions options) {
    BuiltPCH built;
    if(settings.project.readonly == ReadonlyMode::On) {
        co_return built;
    }
    // Through depend, the attempt's edge to the PCH makes a staleness
    // discovery on the shared pair cascade to the document.
    auto wait = [&](PCHRequest request) -> async::task<Result> {
        auto id = pch_node(request.pch_key);
        if(options.attempt) {
            co_return co_await options.attempt->depend(id, pch_work(std::move(request)));
        }
        co_return co_await graph.join(id,
                                      pch_work(std::move(request)),
                                      {.foreground = true, .retry = [] { return true; }});
    };

    // The precompiled header the build declares for the command: the
    // preamble's PCH is built over it, and with none the parse loads it
    // itself. One that did not build leaves the header the command forces
    // in to parse what it holds; so does one the file's rules build
    // otherwise than the writing unit's, which no unit compiles and so
    // nothing indexes, but for a modular parse.
    std::optional<PCHRequest> declared;
    if(auto unit = pch_writer(file, resolution)) {
        declared = declared_pch_request(*unit);
    }
    if(declared && (declared->writer || imports.modular())) {
        auto key = declared->pch_key;
        switch(co_await wait(std::move(*declared))) {
            case Result::Success:
                if(!pch_reads(key, file)) {
                    built.base = std::move(key);
                }
                break;
            case Result::Failed: break;
            case Result::Stale: co_return std::nullopt;
        }
    }

    auto plan = preamble_pch(file,
                             text,
                             point,
                             resolution,
                             imports,
                             built.base.value_or(""),
                             declared.has_value() && !built.base);
    built.partial = plan.partial;
    if(plan.request) {
        auto key = plan.request->pch_key;
        built.preamble = key;
        // A preamble whose build crashed a worker — for this document or
        // another sharing it — is refused until the parse may retry it.
        if(auto* crash = pch_build_crash(key)) {
            if(options.retry_crashed) {
                forget_pch_failure(key);
            } else {
                built.refused = *crash;
            }
        }
        if(!built.refused) {
            // A parse leaving without the index its build opened lets it go:
            // it stays mapped while a compile holds it.
            auto handoff = llvm::make_scope_exit([&] {
                if(auto it = pchs.find(key); it != pchs.end()) {
                    it->second.built_index.reset();
                }
            });
            built.result = co_await wait(std::move(*plan.request));
            if(built.result == Result::Stale) {
                co_return std::nullopt;
            }
            if(built.result == Result::Success && built_pch(key)) {
                built.index = std::move(pchs.find(key)->second.built_index);
            }
        }
    }

    // The declared header precompiled whole covers none of the text.
    if(built.result == Result::Success && built_pch(*built.preamble)) {
        auto& pch = pchs.find(*built.preamble)->second;
        built.loaded = built.preamble;
        built.pch = {pch.path, pch.bound};
    } else if(auto* pch = built.base ? built_pch(*built.base) : nullptr) {
        built.loaded = built.base;
        built.pch = {pch->path, 0};
    } else {
        built.base.reset();
    }
    co_return built;
}

void Project::discard_pch(llvm::StringRef pch_key) {
    auto it = pchs.find(pch_key);
    if(it == pchs.end()) {
        return;
    }
    // An in-flight rebuild owns the entry; its commit publishes a fresh
    // pair in place of the discarded one, so only an idle entry is
    // dropped.
    // FIXME: the rebuild in flight is not restarted: a save that retries a PCH
    // which kept its preamble's errors gets that rebuild's result, still missing
    // a header created after the build looked for it.
    if(!graph.is_compiling(pch_node(pch_key))) {
        forget_pairs(*this, pch_key);
    } else if(store) {
        store->invalidate("pch", it->second.blob);
    }
}

const FailureRecord* Project::pch_build_crash(llvm::StringRef pch_key) {
    auto it = pchs.find(pch_key);
    auto* failed = it != pchs.end() ? it->second.standing_failure(file_table) : nullptr;
    return failed && failed->crashed() ? failed : nullptr;
}

bool Project::forget_pch_failure(llvm::StringRef pch_key) {
    auto it = pchs.find(pch_key);
    return it != pchs.end() && std::exchange(it->second.failure, std::nullopt).has_value();
}

void Project::pch_consume_failed(llvm::StringRef pch_key) {
    if(auto it = pchs.find(pch_key); it != pchs.end() && !it->second.blob.empty() &&
                                     (!store || store->lookup("pch", it->second.blob))) {
        auto& consumes = it->second.consumes;
        if(!consumes) {
            consumes.emplace();
        }
        consumes->fail(pch_consume_policy, {}, {}, FailureRecord::Clock::now());
    }
    discard_pch(pch_key);
}

void Project::pch_consume_succeeded(llvm::StringRef pch_key) {
    if(auto it = pchs.find(pch_key); it != pchs.end()) {
        it->second.consumes.reset();
    }
}

std::shared_ptr<index::TUIndex> Project::preamble_index(llvm::StringRef pch_key) {
    auto it = pchs.find(pch_key);
    if(it == pchs.end()) {
        return nullptr;
    }

    auto& entry = it->second;
    bool had_blob = !entry.index_path.empty();
    auto loaded = entry.load_index();
    if(!loaded && had_blob && store) {
        // The blob was just found unreadable (load_index cleared the
        // path): a pair that looks complete on disk but cannot be opened
        // would be served to every session for the rest of the store's
        // life. Discard it now; the entry itself stays until the next acquisition
        // re-checks the store and rebuilds the pair.
        LOG_WARN("Discarding PCH pair {} with unreadable pch.idx envelope", pch_key);
        store->invalidate("pch", entry.blob);
    }
    return loaded;
}

void Project::forget_evicted_pchs() {
    if(!store) {
        return;
    }
    // An entry mid-rebuild keeps its slot — its commit publishes a fresh
    // pair. A key rebuilt after the eviction was recorded names another
    // blob by now.
    for(auto& evicted: store->take_evictions()) {
        if(evicted.ns != "pch") {
            continue;
        }
        auto it = ranges::find_if(pchs, [&](const auto& entry) {
            return entry.second.blob == evicted.key;
        });
        if(it != pchs.end() && !graph.is_compiling(pch_node(it->getKey()))) {
            forget_pairs(*this, it->getKey());
        }
    }
}

}  // namespace clice

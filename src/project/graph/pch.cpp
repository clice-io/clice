module clice;

import :project.pch;
import :project.precompiled;
import :project.project;
import :support.logging;
import :vfs.file_table;
import :vfs.path;
import :worker.protocol;

namespace clice {

namespace {

/// Forget an entry and discard the pairs it names.
void drop(Project& project, llvm::StringMap<PCHState>::iterator entry) {
    if(project.store) {
        for(auto& blob: {entry->second.blob, entry->second.previous}) {
            if(!blob.empty()) {
                project.store->invalidate("pch", blob);
            }
        }
    }
    project.pch_cache.erase(entry);
}

/// Why the pair registered under the key cannot be adopted as it is; empty
/// when it can: complete, both halves in the store, the PCH on disk, and
/// what it read unchanged. Both halves must be present: a PCH whose pch.idx
/// index is gone (crash between commits, failed aux commit) rebuilds whole.
/// The store lookup refreshes the blob's LRU position, and a passing deps
/// check may repair the snapshot's stat fast path in place. An entry whose
/// blobs the store's LRU evicted, or whose PCH left the disk behind the
/// store's back, goes, or the content-keyed map grows for the server's
/// lifetime.
llvm::StringRef revalidate(Project& project, llvm::StringRef pch_key) {
    auto it = project.pch_cache.find(pch_key);
    if(it == project.pch_cache.end()) {
        return "no_entry";
    }
    auto& st = it->second;
    if(st.path.empty()) {
        return "incomplete_entry";
    }
    if(!project.store || !project.store->lookup("pch", st.blob) ||
       !project.store->lookup_aux("pch", st.blob) || !fs::exists(st.path)) {
        drop(project, it);
        return "evicted";
    }
    // load_state() found the blob unreadable earlier; republish the pair
    // rather than serving a PCH with no index forever.
    if(st.index_path.empty()) {
        return "idx_unreadable";
    }
    if(!st.base.empty()) {
        auto base = project.pch_cache.find(st.base);
        if(base == project.pch_cache.end() || base->second.blob != st.base_blob) {
            return "base_rebuilt";
        }
    }
    if(deps_changed(project.file_table, st.deps)) {
        return "deps_changed";
    }
    return {};
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
    if(auto* failed = project.failures.pch_builds.lookup(project.file_table, pch_key)) {
        if(failed->result.crashed()) {
            LOG_WARN("PCH build for {} refused: key {} crashed a worker", file, pch_key);
        } else {
            LOG_DEBUG("PCH build for {} skipped: key {} failed on these inputs", file, pch_key);
        }
        return true;
    }
    if(auto* consumes = project.failures.pch_consumes.find(pch_key);
       consumes && consumes->result.stops(pch_consume_policy, FailureRecord::Clock::now())) {
        LOG_WARN("PCH build for {} refused: its consumers keep failing to read key {}",
                 file,
                 pch_key);
        return true;
    }
    return false;
}

/// Move a pch key to the front of the loaded-state LRU. Called whenever an
/// entry's pch.idx index is opened or replaced.
void touch_loaded_state(Project& project, llvm::StringRef pch_key) {
    auto& lru = project.loaded_pch_states;
    if(auto it = ranges::find(lru, pch_key); it != lru.end()) {
        lru.erase(it);
    }
    lru.insert(lru.begin(), pch_key.str());
}

/// One attempt: revalidate the registered pair, or dispatch a build with
/// the request's inputs and commit the pair.
async::task<Result> build_pch(Project& project, Attempt& attempt, PCHRequest request) {
    const auto& pch_key = request.pch_key;

    auto pch_miss = revalidate(project, pch_key);
    if(pch_miss.empty()) {
        LOG_PERF("cache", "ns=pch event=hit key={} file={}", pch_key, request.file);
        co_return Result::Success;
    }
    LOG_PERF("cache",
             "ns=pch event=miss reason={} key={} file={}",
             pch_miss,
             pch_key,
             request.file);

    if(!project.store) {
        LOG_WARN("PCH build skipped: cache store is unavailable");
        co_return Result::Failed;
    }
    if(refused(project, pch_key, request.file)) {
        co_return Result::Failed;
    }

    // Build a new pair via a stateless worker: it writes the PCH and its
    // pch.idx index to the tmp paths allocated here; the store commits
    // (fsync + rename) both on success, primary first.
    auto blob = new_blob(pch_key);
    auto pending = project.store->begin_store("pch", blob);
    auto pending_idx = project.store->begin_store_aux("pch", blob);

    worker::BuildPCHParams bp;
    bp.file = std::move(request.file);
    bp.directory = std::move(request.directory);
    bp.project_root = project.root.str();
    bp.arguments = std::move(request.arguments);
    bp.hidden = std::move(request.hidden);
    bp.content = std::move(request.content);
    bp.preamble_bound = request.preamble_bound;
    bp.whole_file = request.whole_file;
    bp.skip_bodies = request.skip_bodies;
    bp.synthesized = std::move(request.synthesized);
    bp.pcms = std::move(request.pcms);
    bp.output_path = pending.tmp_path;
    bp.index_output_path = pending_idx.tmp_path;

    // What the imported modules' PCMs and the PCH built over were built
    // from as the build starts.
    auto before = project.module_inputs(request.modules);
    std::string base_blob;
    if(!request.base.empty()) {
        auto it = project.pch_cache.find(request.base);
        if(it == project.pch_cache.end() || it->second.path.empty()) {
            LOG_DEBUG("PCH build for {} skipped: the PCH it builds over is gone", bp.file);
            co_return Result::Failed;
        }
        bp.pch = it->second.path;
        base_blob = it->second.blob;
        merge_deps(before, {&it->second.deps});
    }
    // What a build that crashes is known to read, sampled before it runs:
    // the modules and the PCH it builds over, and a whole file's own text,
    // which its key leaves out.
    auto known = before;
    if(bp.whole_file) {
        if(auto own = current_dep(project.file_table,
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
        [&](FailureRecord record, DepsSnapshot inputs) {
            project.failures.pch_builds.commit(pch_key, std::move(record), std::move(inputs));
        });
    if(!built) {
        co_return built.error();
    }

    // Commit the pair on the thread pool as one job: the fsyncs stay off
    // the event loop, and the store either publishes the whole pair or
    // discards it (a half pair would hand adopters a PCH whose pch.idx index
    // is gone). Opening the freshly committed blob (mmap + verification,
    // which walks every section's shard) also happens here so no later
    // consumer pays that walk on the event loop.
    struct PairCommit {
        std::optional<std::string> pch_path;
        std::optional<std::string> index_path;
        std::shared_ptr<index::TUIndex> state;
    };

    auto committed = co_await async::queue([&]() -> PairCommit {
        PairCommit commit;
        auto pch_path = project.store->commit(std::move(pending));
        if(!pch_path) {
            // pending_idx cleans its own tmp blob when the frame unwinds.
            return commit;
        }
        commit.pch_path = std::move(*pch_path);

        // The pair is only usable complete: when the index blob cannot be
        // published, discard the PCH too — the next attempt rebuilds both.
        auto index_path = project.store->commit(std::move(pending_idx));
        if(!index_path) {
            project.store->invalidate("pch", blob);
            return commit;
        }
        commit.index_path = std::move(*index_path);
        commit.state = load_pch_index(*commit.index_path);
        // A committed pch.idx index that fails verification is as unusable as
        // an uncommitted one: discard the pair rather than publish a PCH
        // whose preamble state every consumer would fail to load.
        if(!commit.state) {
            project.store->invalidate("pch", blob);
        }
        return commit;
    });
    if(!committed.pch_path.has_value()) {
        LOG_WARN("Failed to commit PCH for {}", bp.file);
        co_return Result::Failed;
    }
    if(!committed.index_path.has_value() || !committed.state) {
        if(!committed.index_path.has_value()) {
            LOG_WARN("Failed to commit pch.idx envelope for {}", bp.file);
        } else {
            LOG_WARN("Freshly committed pch.idx envelope for {} is unreadable", bp.file);
        }
        // The commit job discarded the new pair, and the previous one is
        // stale (that is why it was rebuilt): drop it rather than let it
        // revalidate against its old deps.
        if(auto it = project.pch_cache.find(pch_key); it != project.pch_cache.end()) {
            drop(project, it);
        }
        co_return Result::Failed;
    }

    auto& st = project.pch_cache[pch_key];
    if(!st.previous.empty()) {
        project.store->invalidate("pch", st.previous);
    }
    st.previous = std::exchange(st.blob, std::move(blob));
    st.path = *committed.pch_path;
    st.bound = request.preamble_bound;
    st.base = request.base;
    st.base_blob = std::move(base_blob);
    st.deps = project.read_inputs(*built, before);
    st.index_path = *committed.index_path;
    // Replace the previous blob's mapping (same key, rebuilt content);
    // in-flight holders of the old shared_ptr stay valid.
    st.state = committed.state;
    touch_loaded_state(project, pch_key);
    project.enforce_loaded_budget();

    LOG_INFO("PCH built for {}: {}", bp.file, st.path);

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

const std::shared_ptr<index::TUIndex>& PCHState::load_state() {
    if(!state && !index_path.empty()) {
        state = load_pch_index(index_path);
        if(!state) {
            // Unreadable blob: clear the path so queries don't retry the
            // mmap + verification on every call. The pair now looks
            // incomplete and the next acquisition rebuilds it.
            LOG_WARN("Failed to open pch.idx envelope {}", index_path);
            index_path.clear();
        }
    }
    return state;
}

PCHRequest Project::declared_pch_request(const CommandRef& unit, bool skip_bodies) {
    // The key names the build but the file's content, which the build's
    // inputs cover, its own source among them.
    PCHRequest request{.file = std::string(file_table.resolve(unit.file)),
                       .directory = commands.get(unit.command).directory,
                       .whole_file = true,
                       .skip_bodies = skip_bodies};
    auto rendered = toolchain.render(unit, cdb.spelling(unit.file));
    request.arguments.assign(rendered.argv.begin(), rendered.argv.end());
    request.hidden = std::move(rendered.hidden);
    request.pch_key = cache_key({clang::getClangFullVersion(),
                                 pch_build_rule(skip_bodies),
                                 database_key.str(),
                                 request.directory,
                                 request.file,
                                 canonicalize(request.arguments)});
    return request;
}

bool Project::pch_reads(llvm::StringRef key, Fid file) const {
    auto it = pch_cache.find(key);
    return it != pch_cache.end() && ranges::any_of(it->second.deps, [&](const DepState& dep) {
               return dep.path_id == file;
           });
}

Work Project::pch_work(PCHRequest request) {
    auto shared = std::make_shared<const PCHRequest>(std::move(request));
    return {
        .run = [this, shared](Attempt& attempt) { return build_pch(*this, attempt, *shared); },
        .check =
            [this, shared] {
                return revalidate(*this, shared->pch_key).empty() ? Check::Holds
                                                                  : Check::InputsChanged;
            },
    };
}

async::task<Result> Project::acquire_pch(PCHRequest request) {
    auto id = pch_node(request.pch_key);
    co_return co_await graph.join(id, pch_work(std::move(request)), {.foreground = true});
}

bool Project::pch_building(llvm::StringRef pch_key) const {
    return graph.is_compiling(pch_node(pch_key));
}

void Project::discard_pch(llvm::StringRef pch_key) {
    auto it = pch_cache.find(pch_key);
    if(it == pch_cache.end()) {
        return;
    }
    // An in-flight rebuild owns the entry; its commit publishes a fresh
    // pair in place of the discarded one, so only an idle entry is
    // dropped.
    // FIXME: the rebuild in flight is not restarted: a save that retries a PCH
    // which kept its preamble's errors gets that rebuild's result, still missing
    // a header created after the build looked for it.
    if(!pch_building(pch_key)) {
        drop(*this, it);
    } else if(store) {
        store->invalidate("pch", it->second.blob);
    }
}

const FailureRecord* Project::build_crash(llvm::StringRef pch_key) {
    auto* failed = failures.pch_builds.lookup(file_table, pch_key);
    return failed && failed->result.crashed() ? &failed->result : nullptr;
}

bool Project::forget_build_failure(llvm::StringRef pch_key) {
    return failures.pch_builds.forget(pch_key);
}

void Project::pch_consume_failed(llvm::StringRef pch_key) {
    auto it = pch_cache.find(pch_key);
    bool gone = it == pch_cache.end() || (store && !store->lookup("pch", it->second.blob));
    if(!gone) {
        failures.pch_consumes.entry(pch_key).result.fail(pch_consume_policy,
                                                         {},
                                                         {},
                                                         FailureRecord::Clock::now());
    }
    discard_pch(pch_key);
}

void Project::pch_consume_succeeded(llvm::StringRef pch_key) {
    failures.pch_consumes.forget(pch_key);
}

std::shared_ptr<index::TUIndex> Project::preamble_state(llvm::StringRef pch_key) {
    auto it = pch_cache.find(pch_key);
    if(it == pch_cache.end()) {
        return nullptr;
    }

    auto& st = it->second;
    bool had_blob = !st.index_path.empty();
    auto state = st.load_state();
    if(!state && had_blob && store) {
        // The blob was just found unreadable (load_state cleared the
        // path): a pair that looks complete on disk but cannot be opened
        // would be served to every session for the rest of the store's
        // life. Discard it now; the entry itself stays until the next acquisition
        // re-checks the store and rebuilds the pair.
        LOG_WARN("Discarding PCH pair {} with unreadable pch.idx envelope", pch_key);
        store->invalidate("pch", st.blob);
    }
    if(state) {
        touch_loaded_state(*this, pch_key);
        enforce_loaded_budget();
    }
    return state;
}

void Project::enforce_loaded_budget() {
    // Two keys per open document — its preamble's and the build's
    // precompiled header that preamble is built over — and two extra
    // slots: a closed file's recently used state survives a quick
    // close/reopen, and a shared key serving several documents stays warm
    // while its consumers churn.
    // Open documents' keys always fit the budget, so an unload can only
    // hit keys past the working set; the reload an unlucky consumer then
    // pays (mmap + verification, on the event loop) is the accepted cost
    // of bounding tens of MB per key.
    // Unconnected (tests, tools) assumes a small editor-like working set.
    constexpr std::size_t default_open_documents = 6;
    std::size_t budget = 2 + 2 * (open_documents ? open_documents() : default_open_documents);

    auto& lru = loaded_pch_states;
    std::size_t kept = 0;
    std::size_t i = 0;
    while(i < lru.size()) {
        auto it = pch_cache.find(lru[i]);
        // Erased entries and already-unloaded keys just fall out of the
        // list (invalidation and store eviction bypass the LRU).
        if(it == pch_cache.end() || !it->second.state) {
            lru.erase(lru.begin() + i);
            continue;
        }
        if(kept < budget) {
            kept += 1;
            i += 1;
            continue;
        }
        LOG_DEBUG("Unloading pch.idx envelope of {} (budget {})", lru[i], budget);
        it->second.state.reset();
        lru.erase(lru.begin() + i);
    }
}

}  // namespace clice

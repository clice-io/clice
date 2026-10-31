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

NodeId node(std::uint64_t key_id) {
    return {NodeKind::PCH, key_id};
}

std::uint64_t intern(Project& project, llvm::StringRef pch_key) {
    auto [it, inserted] = project.pch_ids.try_emplace(pch_key, project.pch_requests.size());
    if(inserted) {
        project.pch_requests.emplace_back();
    }
    return it->second;
}

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
/// when it can: complete, both halves in the store, and what it read
/// unchanged. Both halves must be present: a PCH whose pch.idx index is gone
/// (crash between commits, failed aux commit) rebuilds whole. The store
/// lookup refreshes the blob's LRU position, and a passing deps check may
/// repair the snapshot's stat fast path in place. An entry whose blobs the
/// store's LRU evicted goes, or the content-keyed map grows for the
/// server's lifetime.
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
       !project.store->lookup_aux("pch", st.blob)) {
        drop(project, it);
        return "evicted";
    }
    // load_state() found the blob unreadable earlier; republish the pair
    // rather than serving a PCH with no index forever.
    if(st.index_path.empty()) {
        return "idx_unreadable";
    }
    if(deps_changed(project.file_table, st.deps)) {
        return "deps_changed";
    }
    return {};
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
/// the dispatch owner's inputs and commit the pair.
async::task<RoundOutcome> attempt(Project& project, RoundContext& ctx, std::uint64_t key_id) {
    // Copy the dispatch owner's stash: a stale round's retry re-reads it,
    // and the interned-key vector may grow while this frame is suspended,
    // so never hold references into it.
    auto request = project.pch_requests[key_id];
    const auto& pch_key = request.pch_key;

    auto pch_miss = revalidate(project, pch_key);
    if(pch_miss.empty()) {
        LOG_PERF("cache", "ns=pch event=hit key={} file={}", pch_key, request.file);
        co_return RoundOutcome::Success;
    }
    LOG_PERF("cache",
             "ns=pch event=miss reason={} key={} file={}",
             pch_miss,
             pch_key,
             request.file);

    if(!project.store) {
        LOG_WARN("PCH build skipped: cache store is unavailable");
        co_return RoundOutcome::Failed;
    }

    // A build that failed stands while its inputs do: one that crashed a
    // worker is refused before the dispatch, since the PCH is shared and
    // every document with this preamble would burn a worker of its own, and
    // one that failed on the user's code would fail the same way, every
    // compile of every consumer paying for it first. A preamble's key is
    // content-derived: editing the poison starts a fresh key. A
    // precompiled header the build declares is keyed by its command alone:
    // its crash stands until its own file changes, its consumers parsing
    // the header it holds meanwhile. A key whose pairs its consumers keep
    // failing to read stops the same way: rebuilding one would fare no
    // better (see pch_consume_failed).
    if(auto* failed = project.failures.pch_builds.lookup(project.file_table, pch_key)) {
        if(failed->result.crashed()) {
            LOG_WARN("PCH build for {} refused: key {} crashed a worker", request.file, pch_key);
        } else {
            LOG_DEBUG("PCH build for {} skipped: key {} failed on these inputs",
                      request.file,
                      pch_key);
        }
        co_return RoundOutcome::Failed;
    }
    if(auto* consumes = project.failures.pch_consumes.find(pch_key);
       consumes && consumes->result.stops(pch_consume_policy, FailureRecord::Clock::now())) {
        LOG_WARN("PCH build for {} refused: its consumers keep failing to read key {}",
                 request.file,
                 pch_key);
        co_return RoundOutcome::Failed;
    }

    // Build a new pair via a stateless worker: it writes the PCH and its
    // pch.idx index to the tmp paths allocated here; the store commits
    // (fsync + rename) both on success, primary first.
    std::random_device random;
    auto blob = std::format("{}-{:08x}{:08x}", pch_key, random(), random());
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
    if(!request.base.empty()) {
        auto it = project.pch_cache.find(request.base);
        if(it == project.pch_cache.end() || it->second.path.empty()) {
            LOG_DEBUG("PCH build for {} skipped: the PCH it builds over is gone", bp.file);
            co_return RoundOutcome::Failed;
        }
        bp.pch = it->second.path;
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
    auto built = co_await send_precompiled(
        project,
        ctx,
        bp,
        "PCH",
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
        PairCommit outcome;
        auto pch_path = project.store->commit(std::move(pending));
        if(!pch_path) {
            // pending_idx cleans its own tmp blob when the frame unwinds.
            return outcome;
        }
        outcome.pch_path = std::move(*pch_path);

        // The pair is only usable complete: when the index blob cannot be
        // published, discard the PCH too — the next round rebuilds both.
        auto index_path = project.store->commit(std::move(pending_idx));
        if(!index_path) {
            project.store->invalidate("pch", blob);
            return outcome;
        }
        outcome.index_path = std::move(*index_path);
        outcome.state = load_pch_index(*outcome.index_path);
        // A committed pch.idx index that fails verification is as unusable as
        // an uncommitted one: discard the pair rather than publish a PCH
        // whose preamble state every consumer would fail to load.
        if(!outcome.state) {
            project.store->invalidate("pch", blob);
        }
        return outcome;
    });
    if(!committed.pch_path.has_value()) {
        LOG_WARN("Failed to commit PCH for {}", bp.file);
        co_return RoundOutcome::Failed;
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
        co_return RoundOutcome::Failed;
    }

    auto& st = project.pch_cache[pch_key];
    if(!st.previous.empty()) {
        project.store->invalidate("pch", st.previous);
    }
    st.previous = std::exchange(st.blob, std::move(blob));
    st.path = *committed.pch_path;
    st.bound = request.preamble_bound;
    st.base = request.base;
    st.deps = read_inputs(project, *built, before);
    st.index_path = *committed.index_path;
    // Replace the previous blob's mapping (same key, rebuilt content);
    // in-flight holders of the old shared_ptr stay valid.
    st.state = committed.state;
    touch_loaded_state(project, pch_key);
    enforce_loaded_budget(project);

    LOG_INFO("PCH built for {}: {}", bp.file, st.path);

    project.mark_precompiled_dirty();

    co_return RoundOutcome::Success;
}

}  // namespace

PCHRequest declared_pch_request(Project& project, const CommandRef& unit, bool skip_bodies) {
    // The key names the build but the file's content, which the build's
    // inputs cover, its own source among them.
    PCHRequest request{.file = std::string(project.file_table.resolve(unit.file)),
                       .directory = project.commands.get(unit.command).directory,
                       .whole_file = true,
                       .skip_bodies = skip_bodies};
    auto rendered = project.toolchain.render(unit, project.cdb.spelling(unit.file));
    request.arguments.assign(rendered.argv.begin(), rendered.argv.end());
    request.hidden = std::move(rendered.hidden);
    request.pch_key = cache_key({clang::getClangFullVersion(),
                                 pch_build_rule(skip_bodies),
                                 project.database_key.str(),
                                 request.directory,
                                 request.file,
                                 canonicalize(request.arguments)});
    return request;
}

bool pch_reads(const Project& project, llvm::StringRef key, Fid file) {
    auto it = project.pch_cache.find(key);
    return it != project.pch_cache.end() &&
           ranges::any_of(it->second.deps,
                          [&](const DepState& dep) { return dep.path_id == file; });
}

NodeId prepare_pch(Project& project, PCHRequest request) {
    auto key_id = intern(project, request.pch_key);
    auto id = node(key_id);
    auto& graph = project.graph;

    // A clean node can go stale behind the graph's back: LRU eviction,
    // deps drift, a discarded pair. Re-dirty it so a fresh round
    // revalidates or rebuilds.
    if(graph.has_node(id) && !graph.is_dirty(id) && !graph.is_compiling(id) &&
       !revalidate(project, request.pch_key).empty()) {
        graph.update(id);
    }

    // The caller's join spawns a round synchronously only for a dirty
    // node with no round live (a missing node is created dirty) — that
    // caller is the dispatch owner: stash its inputs for the round. A
    // clean fresh node's join consumes nothing, and a stash left there
    // would pin the request's buffer in the monotonic table for as long as
    // the key stays fresh.
    if(!graph.is_compiling(id) && (!graph.has_node(id) || graph.is_dirty(id))) {
        project.pch_requests[key_id] = std::move(request);
    }
    return id;
}

async::task<PCHOutcome> acquire_pch(Project& project, PCHRequest request) {
    auto id = prepare_pch(project, std::move(request));
    switch(co_await project.graph.request(id, {.kind = JoinKind::OneAttempt, .foreground = true})) {
        case JoinOutcome::Success: co_return PCHOutcome::Ready;
        case JoinOutcome::Failed: co_return PCHOutcome::Failed;
        case JoinOutcome::Stale:
        case JoinOutcome::Abandoned:
        case JoinOutcome::Shutdown: co_return PCHOutcome::Preempted;
    }
    std::unreachable();
}

async::task<RoundOutcome> pch_round(Project& project, RoundContext& ctx, NodeId id) {
    auto outcome = co_await attempt(project, ctx, id.key);
    // A stale round's replacement respawns through join_node without
    // passing prepare_pch(), so attempt() copies the stash instead of
    // consuming it. Retire it on a current round's result, or once nobody
    // waits on the key: an overtaken or preempted round its waiters retry
    // still needs the inputs, one that lost every waiter (the preamble
    // being typed moved on to another key) never runs again.
    if((outcome != RoundOutcome::Stale && ctx.current()) || project.graph.refcount(id) == 0) {
        project.pch_requests[id.key] = {};
    }
    co_return outcome;
}

bool pch_building(const Project& project, llvm::StringRef pch_key) {
    auto it = project.pch_ids.find(pch_key);
    return it != project.pch_ids.end() && project.graph.is_compiling(node(it->second));
}

void discard_pch(Project& project, llvm::StringRef pch_key) {
    auto it = project.pch_cache.find(pch_key);
    if(it == project.pch_cache.end()) {
        return;
    }
    // An in-flight rebuild owns the entry; its commit publishes a fresh
    // pair in place of the discarded one, so only an idle entry is
    // dropped.
    // FIXME: the rebuild in flight is not restarted: a save that retries a PCH
    // which kept its preamble's errors gets that rebuild's result, still missing
    // a header created after the build looked for it.
    if(!pch_building(project, pch_key)) {
        drop(project, it);
    } else if(project.store) {
        project.store->invalidate("pch", it->second.blob);
    }
}

const FailureRecord* build_crash(Project& project, llvm::StringRef pch_key) {
    auto* failed = project.failures.pch_builds.lookup(project.file_table, pch_key);
    return failed && failed->result.crashed() ? &failed->result : nullptr;
}

bool forget_build_failure(Project& project, llvm::StringRef pch_key) {
    return project.failures.pch_builds.forget(pch_key);
}

void pch_consume_failed(Project& project, llvm::StringRef pch_key) {
    auto it = project.pch_cache.find(pch_key);
    bool gone = it == project.pch_cache.end() ||
                (project.store && !project.store->lookup("pch", it->second.blob));
    if(!gone) {
        project.failures.pch_consumes.entry(pch_key).result.fail(pch_consume_policy,
                                                                 {},
                                                                 {},
                                                                 FailureRecord::Clock::now());
    }
    discard_pch(project, pch_key);
}

void pch_consume_succeeded(Project& project, llvm::StringRef pch_key) {
    project.failures.pch_consumes.forget(pch_key);
}

std::shared_ptr<index::TUIndex> preamble_state(Project& project, llvm::StringRef pch_key) {
    auto it = project.pch_cache.find(pch_key);
    if(it == project.pch_cache.end()) {
        return nullptr;
    }

    auto& st = it->second;
    bool had_blob = !st.index_path.empty();
    auto state = st.load_state();
    if(!state && had_blob && project.store) {
        // The blob was just found unreadable (load_state cleared the
        // path): a pair that looks complete on disk but cannot be opened
        // would be served to every session for the rest of the store's
        // life. Discard it now; the entry itself stays until ensure_pch
        // re-checks the store and rebuilds the pair.
        LOG_WARN("Discarding PCH pair {} with unreadable pch.idx envelope", pch_key);
        project.store->invalidate("pch", st.blob);
    }
    if(state) {
        touch_loaded_state(project, pch_key);
        enforce_loaded_budget(project);
    }
    return state;
}

void enforce_loaded_budget(Project& project) {
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
    std::size_t budget =
        2 + 2 * (project.open_documents ? project.open_documents() : default_open_documents);

    auto& lru = project.loaded_pch_states;
    std::size_t kept = 0;
    std::size_t i = 0;
    while(i < lru.size()) {
        auto it = project.pch_cache.find(lru[i]);
        // Erased entries and already-unloaded keys just fall out of the
        // list (invalidation and store eviction bypass the LRU).
        if(it == project.pch_cache.end() || !it->second.state) {
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

std::size_t stashed_pch_bytes(const Project& project) {
    std::size_t bytes = 0;
    for(auto& request: project.pch_requests) {
        bytes += request.content.size();
    }
    return bytes;
}

}  // namespace clice

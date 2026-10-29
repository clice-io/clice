module clice;

import :sched.families.build_common;
import :sched.families.pch;
import :support.anomaly;
import :support.logging;
import :vfs.file_table;
import :worker.protocol;

namespace clice {

PCHFamily::PCHFamily(TaskGraph& graph, Project& project, WorkerPool& pool) :
    graph(graph), project(project), pool(pool) {}

void PCHFamily::register_runner() {
    graph.register_family(Family::PCH,
                          [this](RoundContext& ctx, NodeId id) { return run(ctx, id.key); });
}

std::uint64_t PCHFamily::intern(llvm::StringRef pch_key) {
    auto [it, inserted] = ids.try_emplace(pch_key, states.size());
    if(inserted) {
        states.emplace_back();
    }
    return it->second;
}

PCHFamily::Request PCHFamily::precompiled_header(const CommandRef& unit, bool skip_bodies) {
    // The key names the build but the file's content, which the build's
    // inputs cover, its own source among them.
    Request request{.file = std::string(project.file_table.resolve(unit.file)),
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

bool PCHFamily::reads(llvm::StringRef key, Fid file) const {
    auto it = project.pch_cache.find(key);
    return it != project.pch_cache.end() &&
           ranges::any_of(it->second.deps,
                          [&](const DepState& dep) { return dep.path_id == file; });
}

NodeId PCHFamily::prepare(Request request) {
    auto key_id = intern(request.pch_key);
    auto id = node(key_id);

    // A clean node can go stale behind the graph's back: LRU eviction,
    // deps drift, a retracted pair. Re-dirty it so a fresh round
    // revalidates or rebuilds.
    if(graph.has_node(id) && !graph.is_dirty(id) && !graph.is_compiling(id) &&
       !fresh(request.pch_key)) {
        graph.update(id);
    }

    // The caller's join spawns a round synchronously only for a dirty
    // node with no round live (a missing node is created dirty) — that
    // caller is the dispatch owner: stash its inputs for the round. A
    // clean fresh node's join consumes nothing, and a stash left there
    // would pin the request's buffer in the monotonic table for as long as
    // the key stays fresh.
    if(!graph.is_compiling(id) && (!graph.has_node(id) || graph.is_dirty(id))) {
        states[key_id] = {std::move(request)};
    }
    return id;
}

kota::task<PCHFamily::Outcome> PCHFamily::acquire(Request request) {
    auto id = prepare(std::move(request));

    switch(co_await graph.request(id, {.kind = JoinKind::OneAttempt, .foreground = true})) {
        case JoinOutcome::Success: co_return Outcome::Ready;
        case JoinOutcome::Failed: co_return Outcome::Failed;
        case JoinOutcome::Stale:
        case JoinOutcome::Abandoned:
        case JoinOutcome::Shutdown: co_return Outcome::Preempted;
    }
    std::unreachable();
}

kota::task<RoundOutcome> PCHFamily::run(RoundContext& ctx, std::uint64_t key_id) {
    auto outcome = co_await attempt(ctx, key_id);
    // A stale round's replacement respawns through join_node without
    // passing prepare(), so attempt() copies the stash instead of
    // consuming it. Retire it on a current round's result, or once nobody
    // waits on the key: an overtaken or preempted round its waiters retry
    // still needs the inputs, one that lost every waiter (the preamble
    // being typed moved on to another key) never runs again.
    if((outcome != RoundOutcome::Stale && ctx.current()) || graph.refcount(node(key_id)) == 0) {
        states[key_id] = {};
    }
    co_return outcome;
}

kota::task<RoundOutcome> PCHFamily::attempt(RoundContext& ctx, std::uint64_t key_id) {
    // Copy the dispatch owner's stash: a stale round's retry re-reads it,
    // and the interned-key vector may grow while this frame is suspended,
    // so never hold references into it.
    auto request = states[key_id].inputs;
    const auto& pch_key = request.pch_key;

    // Authoritative revalidation of the registered pair. Both halves must
    // be present in the store: a PCH whose pch.idx index is gone
    // (crash between commits, failed aux commit) rebuilds whole. The
    // store lookup refreshes the blob's LRU position.
    llvm::StringRef pch_miss = "no_entry";
    if(auto it = project.pch_cache.find(pch_key); it != project.pch_cache.end()) {
        auto& st = it->second;
        bool in_store = project.store && project.store->lookup("pch", st.blob) &&
                        project.store->lookup_aux("pch", st.blob);
        if(st.path.empty()) {
            pch_miss = "incomplete_entry";
        } else if(!in_store) {
            pch_miss = "evicted";
        } else if(st.index_path.empty()) {
            // load_state() found the blob unreadable earlier; republish
            // the pair rather than serving a PCH with no index forever.
            pch_miss = "idx_unreadable";
        } else if(deps_changed(project.file_table, st.deps)) {
            pch_miss = "deps_changed";
        } else {
            LOG_PERF("cache", "ns=pch event=hit key={} file={}", pch_key, request.file);
            co_return RoundOutcome::Success;
        }
        // Blob evicted by the store's LRU: drop the metadata too, or
        // the content-keyed map grows for the server's lifetime.
        if(!in_store) {
            drop(it);
        }
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

    // A preamble whose PCH build killed a worker is refused before the
    // dispatch: the artifact is shared, so one document's quarantine
    // cannot contain it — every session with this preamble would burn a
    // worker of its own. A preamble's key is content-derived: editing the
    // poison starts a fresh key. A precompiled header the build declares
    // is keyed by its command alone, so its refusal lasts the session:
    // its consumers parse the header it holds instead. Consumption strikes
    // park the key the same way: rebuilding a pair whose every rebuild gets
    // blamed again would fare no better (see blame).
    if(build_crashes.contains(pch_key)) {
        LOG_WARN("PCH build for {} refused: key {} crashed a worker", request.file, pch_key);
        co_return RoundOutcome::Failed;
    }
    if(consume_blames.blocked(pch_key)) {
        LOG_WARN("PCH build for {} refused: key {} keeps getting blamed by its consumers",
                 request.file,
                 pch_key);
        co_return RoundOutcome::Failed;
    }
    if(auto it = build_failures.find(pch_key); it != build_failures.end()) {
        if(!deps_changed(project.file_table, it->second)) {
            LOG_DEBUG("PCH build for {} skipped: key {} failed on these inputs",
                      request.file,
                      pch_key);
            co_return RoundOutcome::Failed;
        }
        build_failures.erase(it);
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
    bp.workspace = project.config.workspace_root.str();
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
    // from as the build starts: one rebuilt meanwhile stales the PCH.
    auto imported = project.module_inputs(request.modules);
    DepsSnapshot base;
    if(!request.base.empty()) {
        auto it = project.pch_cache.find(request.base);
        if(it == project.pch_cache.end() || it->second.path.empty()) {
            LOG_DEBUG("PCH build for {} skipped: the PCH it builds over is gone", bp.file);
            co_return RoundOutcome::Failed;
        }
        bp.pch = it->second.path;
        base = it->second.deps;
    }
    auto inputs = [&](const worker::ArtifactBuildResult& built) {
        auto deps = capture_deps_snapshot(project.file_table, built.deps, built.build_at);
        merge_deps(deps, {&imported, &base});
        return deps;
    };

    LOG_DEBUG("Building PCH for {}, bound={}, key={}", bp.file, bp.preamble_bound, pch_key);

    // The advisory token rides into the pool, which cancels the request
    // over IPC while this frame keeps awaiting the real reply
    // (contract 2). A crash is recorded on the key; its
    // consumers book it on their documents when they find it there.
    // A background index unit waiting on a precompiled header the build
    // declares leaves the interactive work its priority.
    auto priority = ctx.foreground() ? worker::Priority::High : worker::Priority::Low;
    auto result = co_await deliver(
        pool,
        false,
        [&] { return pool.send_stateless(bp, priority, ctx.token()); },
        [&](const ipc::Error& error) { build_crashes.insert_or_assign(pch_key, error); });

    if(!result.has_value() && result.error().code == worker::dispatch_errc::cancelled) {
        LOG_INFO("PCH build preempted for {}, will retry", bp.file);
        co_return RoundOutcome::Stale;
    }
    if(!result.has_value() || !result.value().success) {
        if(expected_build_failure(result)) {
            LOG_WARN("PCH build failed for {}: {}", bp.file, build_failure_message(result));
            if(result.has_value()) {
                build_failures.insert_or_assign(pch_key, inputs(result.value()));
            }
        } else {
            LOG_ANOMALY(PCHBuildFail,
                        "PCH build failed for {}: {}",
                        bp.file,
                        build_failure_message(result));
        }
        co_return RoundOutcome::Failed;
    }

    // Commit the pair on the thread pool as one job: the fsyncs stay off
    // the event loop, and the store either publishes the whole pair or
    // retracts it (a half pair would hand adopters a PCH whose pch.idx index
    // is gone). Opening the freshly committed blob (mmap + verification,
    // which walks every section's shard) also happens here so no later
    // consumer pays that walk on the event loop.
    struct PairCommit {
        std::optional<std::string> pch_path;
        std::optional<std::string> index_path;
        std::shared_ptr<index::TUIndex> state;
    };

    auto committed = co_await kota::queue([&]() -> PairCommit {
        PairCommit outcome;
        auto pch_path = project.store->commit(std::move(pending));
        if(!pch_path) {
            // pending_idx cleans its own tmp blob when the frame unwinds.
            return outcome;
        }
        outcome.pch_path = std::move(*pch_path);

        // The pair is only usable complete: when the index blob cannot be
        // published, retract the PCH too — the next round rebuilds both.
        auto index_path = project.store->commit(std::move(pending_idx));
        if(!index_path) {
            project.store->invalidate("pch", blob);
            return outcome;
        }
        outcome.index_path = std::move(*index_path);
        outcome.state = load_pch_index(*outcome.index_path);
        // A committed pch.idx index that fails verification is as unusable as
        // an uncommitted one: retract the pair rather than publish a PCH
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
    if(!committed.index_path.has_value()) {
        LOG_WARN("Failed to commit pch.idx envelope for {}", bp.file);
        // The previous pair is stale (that is why it was rebuilt): drop
        // it rather than let it revalidate against its old deps.
        if(auto it = project.pch_cache.find(pch_key); it != project.pch_cache.end()) {
            drop(it);
        }
        co_return RoundOutcome::Failed;
    }
    if(!committed.state) {
        LOG_WARN("Freshly committed pch.idx envelope for {} is unreadable", bp.file);
        // The commit job retracted the new pair; the previous one is stale
        // for the same reason as above.
        if(auto it = project.pch_cache.find(pch_key); it != project.pch_cache.end()) {
            drop(it);
        }
        co_return RoundOutcome::Failed;
    }

    auto& st = project.pch_cache[pch_key];
    if(!st.superseded.empty()) {
        project.store->invalidate("pch", st.superseded);
    }
    st.superseded = std::exchange(st.blob, std::move(blob));
    st.path = *committed.pch_path;
    st.bound = request.preamble_bound;
    st.base = request.base;
    st.deps = inputs(result.value());
    st.index_path = *committed.index_path;
    // Replace the previous blob's mapping (same key, rebuilt content);
    // in-flight holders of the old shared_ptr stay valid.
    st.state = committed.state;
    touch_loaded_state(pch_key);
    enforce_loaded_budget();

    LOG_INFO("PCH built for {}: {}", bp.file, st.path);

    project.mark_artifacts_dirty();

    co_return RoundOutcome::Success;
}

bool PCHFamily::fresh(llvm::StringRef pch_key) {
    auto it = project.pch_cache.find(pch_key);
    if(it == project.pch_cache.end() || it->second.path.empty() || it->second.index_path.empty()) {
        return false;
    }
    auto& st = it->second;
    if(!project.store || !project.store->lookup("pch", st.blob) ||
       !project.store->lookup_aux("pch", st.blob)) {
        return false;
    }
    return !deps_changed(project.file_table, st.deps);
}

bool PCHFamily::building(llvm::StringRef pch_key) const {
    auto it = ids.find(pch_key);
    return it != ids.end() && graph.is_compiling(node(it->second));
}

void PCHFamily::drop(llvm::StringMap<PCHState>::iterator entry) {
    if(project.store) {
        for(auto& blob: {entry->second.blob, entry->second.superseded}) {
            if(!blob.empty()) {
                project.store->invalidate("pch", blob);
            }
        }
    }
    project.pch_cache.erase(entry);
}

void PCHFamily::invalidate(llvm::StringRef pch_key) {
    auto it = project.pch_cache.find(pch_key);
    if(it == project.pch_cache.end()) {
        return;
    }
    // An in-flight rebuild owns the entry; its commit publishes a fresh
    // pair in place of the retracted one, so only an idle entry is
    // dropped.
    // FIXME: the rebuild in flight is not restarted: a save that retries a PCH
    // which kept its preamble's errors gets that rebuild's result, still missing
    // a header created after the build looked for it.
    if(!building(pch_key)) {
        drop(it);
    } else if(project.store) {
        project.store->invalidate("pch", it->second.blob);
    }
}

void PCHFamily::blame(llvm::StringRef pch_key) {
    auto it = project.pch_cache.find(pch_key);
    bool gone = it == project.pch_cache.end() ||
                (project.store && !project.store->lookup("pch", it->second.blob));
    if(!gone) {
        consume_blames.on_blame(pch_key);
    }
    invalidate(pch_key);
}

std::shared_ptr<index::TUIndex> PCHFamily::preamble_state(llvm::StringRef pch_key) {
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
        // life. Retract it now; the entry itself stays until ensure_pch
        // re-checks the store and rebuilds the pair.
        LOG_WARN("Retracting PCH pair {} with unreadable pch.idx envelope", pch_key);
        project.store->invalidate("pch", st.blob);
    }
    if(state) {
        touch_loaded_state(pch_key);
        enforce_loaded_budget();
    }
    return state;
}

void PCHFamily::touch_loaded_state(llvm::StringRef pch_key) {
    auto it = ranges::find(loaded_state_lru, pch_key);
    if(it != loaded_state_lru.end()) {
        loaded_state_lru.erase(it);
    }
    loaded_state_lru.insert(loaded_state_lru.begin(), pch_key.str());
}

void PCHFamily::enforce_loaded_budget() {
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

    std::size_t kept = 0;
    std::size_t i = 0;
    while(i < loaded_state_lru.size()) {
        auto it = project.pch_cache.find(loaded_state_lru[i]);
        // Erased entries and already-unloaded keys just fall out of the
        // list (invalidation and store eviction bypass the LRU).
        if(it == project.pch_cache.end() || !it->second.state) {
            loaded_state_lru.erase(loaded_state_lru.begin() + i);
            continue;
        }
        if(kept < budget) {
            kept += 1;
            i += 1;
            continue;
        }
        LOG_DEBUG("Unloading pch.idx envelope of {} (budget {})", loaded_state_lru[i], budget);
        it->second.state.reset();
        loaded_state_lru.erase(loaded_state_lru.begin() + i);
    }
}

}  // namespace clice

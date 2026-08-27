#include "sched/families/pch.h"

#include <utility>

#include "sched/families/build_common.h"
#include "support/anomaly.h"
#include "support/logging.h"
#include "worker/protocol.h"

namespace clice {

PCHFamily::PCHFamily(TaskGraph& graph,
                     Workspace& workspace,
                     ContextResolver& contexts,
                     WorkerPool& pool) :
    graph(graph), workspace(workspace), contexts(contexts), pool(pool) {}

void PCHFamily::register_runner() {
    graph.register_family(pch_family,
                          [this](RoundContext& ctx, NodeId id) { return run(ctx, id.key); });
}

std::uint64_t PCHFamily::intern(llvm::StringRef pch_key) {
    auto [it, inserted] = ids.try_emplace(pch_key, states.size());
    if(inserted) {
        states.emplace_back();
    }
    return it->second;
}

NodeId PCHFamily::prepare(Request request, std::function<void(llvm::StringRef)> on_crash) {
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
    // caller is the dispatch owner: stash its inputs and probe for the
    // round. A clean fresh node's join consumes nothing, and a stash left
    // there would pin the request's buffer and the probe's captured
    // session in the monotonic table for as long as the key stays fresh.
    if(!graph.is_compiling(id) && (!graph.has_node(id) || graph.is_dirty(id))) {
        states[key_id] = {std::move(request), std::move(on_crash)};
    }
    return id;
}

kota::task<PCHFamily::Outcome> PCHFamily::acquire(Request request,
                                                  std::function<void(llvm::StringRef)> on_crash) {
    auto id = prepare(std::move(request), std::move(on_crash));

    switch(co_await graph.request(id, {.flavor = JoinFlavor::OneAttempt, .foreground = true})) {
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
    // consuming it. Retire it only on a current round's verdict — an
    // overtaken or preempted round is about to be retried and the retry
    // still needs the inputs.
    if(outcome != RoundOutcome::Stale && ctx.current()) {
        states[key_id] = {};
    }
    co_return outcome;
}

kota::task<RoundOutcome> PCHFamily::attempt(RoundContext& ctx, std::uint64_t key_id) {
    // Copy the dispatch owner's stash: a stale round's retry re-reads it,
    // and the interned-key vector may grow while this frame is suspended,
    // so never hold references into it.
    auto request = states[key_id].inputs;
    auto probe = states[key_id].on_crash;
    const auto& pch_key = request.pch_key;

    // Authoritative revalidation of the registered pair. Both halves must
    // be present in the store: a PCH whose pch.idx envelope is gone
    // (crash between commits, failed aux commit) rebuilds whole. The
    // store lookup refreshes the blob's LRU position.
    llvm::StringRef pch_miss = "no_entry";
    if(auto it = workspace.pch_cache.find(pch_key); it != workspace.pch_cache.end()) {
        auto& st = it->second;
        bool in_store = workspace.store && workspace.store->lookup("pch", pch_key) &&
                        workspace.store->lookup_aux("pch", pch_key);
        if(st.path.empty()) {
            pch_miss = "incomplete_entry";
        } else if(!in_store) {
            pch_miss = "evicted";
        } else if(st.index_path.empty()) {
            // load_state() found the blob unreadable earlier; republish
            // the pair rather than serving a PCH with no index forever.
            pch_miss = "idx_unreadable";
        } else if(deps_changed(workspace.path_pool, st.deps)) {
            pch_miss = "deps_changed";
        } else {
            LOG_PERF("cache", "ns=pch event=hit key={} file={}", pch_key, request.file);
            co_return RoundOutcome::Success;
        }
        // Blob evicted by the store's LRU: drop the metadata too, or the
        // content-keyed map grows for the server's lifetime.
        if(!in_store) {
            workspace.pch_cache.erase(it);
        }
    }
    LOG_PERF("cache",
             "ns=pch event=miss reason={} key={} file={}",
             pch_miss,
             pch_key,
             request.file);

    if(!workspace.store) {
        LOG_WARN("PCH build skipped: cache store is unavailable");
        co_return RoundOutcome::Failed;
    }

    // A preamble whose PCH build keeps killing workers is refused before
    // the dispatch: the artifact is shared, so one document's quarantine
    // cannot contain it — every session with this preamble would burn
    // workers of its own. The key is content-derived: editing the poison
    // starts a fresh key with a fresh budget. Consumption strikes park
    // the key the same way: rebuilding a pair whose every rebuild gets
    // blamed again would fare no better (see blame).
    if(workspace.build_crashes.blocked(pch_key)) {
        LOG_WARN("PCH build for {} refused: key {} keeps crashing workers", request.file, pch_key);
        co_return RoundOutcome::Failed;
    }
    if(consume_blames.blocked(pch_key)) {
        LOG_WARN("PCH build for {} refused: key {} keeps getting blamed by its consumers",
                 request.file,
                 pch_key);
        co_return RoundOutcome::Failed;
    }

    // Build a new pair via a stateless worker: it writes the PCH and its
    // pch.idx envelope to the tmp paths allocated here; the store commits
    // (fsync + rename) both on success, primary first.
    auto pending = workspace.store->begin_store("pch", pch_key);
    auto pending_idx = workspace.store->begin_store_aux("pch", pch_key);

    worker::BuildPCHParams bp;
    bp.file = std::move(request.file);
    bp.directory = std::move(request.directory);
    bp.arguments = std::move(request.arguments);
    bp.content = std::move(request.content);
    bp.preamble_bound = request.preamble_bound;
    bp.output_path = pending.tmp_path;
    bp.index_output_path = pending_idx.tmp_path;

    LOG_DEBUG("Building PCH for {}, bound={}, key={}", bp.file, bp.preamble_bound, pch_key);

    // Each worker kill lands in two ledgers by design: the shared key's
    // (other sessions with the same preamble must stop re-triggering the
    // build) and, through the owner's probe, the owning document's (the
    // preamble is that document's content).
    auto crashed = [&](const kota::ipc::protocol::Error& error) {
        workspace.build_crashes.on_crash(pch_key);
        probe(worker::death_of(error));
    };

    // The advisory token rides into the pool, which translates a fire
    // into the cooperative CancelBuild while this frame keeps awaiting
    // the real reply (contract 2). One resend if the worker died: the
    // pool marks the dead slot, so the retry lands on a healthy worker; a
    // request that kills two workers in a row is a poison workload a
    // third attempt would not survive either.
    auto result = co_await pool.send_stateless(bp, worker::Priority::High, {}, ctx.token());
    if(!result.has_value() && result.error().code == worker::dispatch_errc::worker_crashed) {
        crashed(result.error());
        result = co_await pool.send_stateless(bp, worker::Priority::High, {}, ctx.token());
        if(!result.has_value() && result.error().code == worker::dispatch_errc::worker_crashed) {
            crashed(result.error());
        }
    }

    if(!result.has_value() && result.error().code == worker::dispatch_errc::cancelled) {
        LOG_INFO("PCH build preempted for {}, will retry", bp.file);
        co_return RoundOutcome::Stale;
    }
    if(!result.has_value() || !result.value().success) {
        if(expected_build_failure(result)) {
            LOG_WARN("PCH build failed for {}: {}", bp.file, build_failure_message(result));
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
    // retracts it (a half pair would hand adopters a PCH whose envelope
    // is gone). Opening the freshly committed blob (mmap + flatbuffer
    // verification, which walks the whole file) also happens here so no
    // later consumer pays that walk on the event loop.
    struct PairCommit {
        std::optional<std::string> pch_path;
        std::optional<std::string> index_path;
        std::shared_ptr<index::TUIndex> state;
    };

    auto committed = co_await kota::queue([&]() -> PairCommit {
        PairCommit outcome;
        auto pch_path = workspace.store->commit(std::move(pending));
        if(!pch_path) {
            // pending_idx cleans its own tmp blob when the frame unwinds.
            return outcome;
        }
        outcome.pch_path = std::move(*pch_path);

        // The pair is only usable complete: when the index blob cannot be
        // published, retract the PCH too — the next round rebuilds both.
        auto index_path = workspace.store->commit(std::move(pending_idx));
        if(!index_path) {
            workspace.store->invalidate("pch", pch_key);
            return outcome;
        }
        outcome.index_path = std::move(*index_path);
        outcome.state = load_pch_envelope(*outcome.index_path);
        // A committed envelope that fails verification is as unusable as
        // an uncommitted one: retract the pair rather than publish a PCH
        // whose preamble state every consumer would fail to load.
        if(!outcome.state) {
            workspace.store->invalidate("pch", pch_key);
        }
        return outcome;
    });
    if(!committed.has_value() || !committed.value().pch_path.has_value()) {
        LOG_WARN("Failed to commit PCH for {}", bp.file);
        co_return RoundOutcome::Failed;
    }
    if(!committed.value().index_path.has_value()) {
        LOG_WARN("Failed to commit pch.idx envelope for {}", bp.file);
        // A rebuild of an existing key just had its blobs retracted from
        // the store; the entry's paths now dangle and a settled entry
        // would revalidate as a hit against deleted blobs. Drop it.
        workspace.pch_cache.erase(pch_key);
        co_return RoundOutcome::Failed;
    }
    if(!committed.value().state) {
        LOG_WARN("Freshly committed pch.idx envelope for {} is unreadable", bp.file);
        // The commit job retracted the pair; drop the entry for the same
        // dangling-paths reason as above.
        workspace.pch_cache.erase(pch_key);
        co_return RoundOutcome::Failed;
    }

    // The key built: its strikes were transient, not poison. The shared
    // account clears unconditionally; per-document ledgers clear on the
    // adoption side, each joiner for itself, gated on its own validity.
    workspace.build_crashes.on_land(pch_key);

    auto& st = workspace.pch_cache[pch_key];
    st.path = *committed.value().pch_path;
    st.bound = request.preamble_bound;
    st.deps =
        capture_deps_snapshot(workspace.path_pool, result.value().deps, result.value().build_at);
    st.index_path = *committed.value().index_path;
    // Replace the previous blob's mapping (same key, rebuilt content);
    // in-flight holders of the old shared_ptr stay valid.
    st.state = committed.value().state;
    workspace.touch_loaded_state(pch_key);
    workspace.enforce_loaded_budget();

    LOG_INFO("PCH built for {}: {}", bp.file, st.path);

    // Persist cache metadata after successful build.
    workspace.save_cache(contexts);

    co_return RoundOutcome::Success;
}

bool PCHFamily::fresh(llvm::StringRef pch_key) {
    auto it = workspace.pch_cache.find(pch_key);
    if(it == workspace.pch_cache.end() || it->second.path.empty() ||
       it->second.index_path.empty()) {
        return false;
    }
    if(!workspace.store || !workspace.store->lookup("pch", pch_key) ||
       !workspace.store->lookup_aux("pch", pch_key)) {
        return false;
    }
    return !deps_changed(workspace.path_pool, it->second.deps);
}

bool PCHFamily::building(llvm::StringRef pch_key) const {
    auto it = ids.find(pch_key);
    return it != ids.end() && graph.is_compiling(node(it->second));
}

void PCHFamily::invalidate(llvm::StringRef pch_key) {
    if(workspace.store) {
        workspace.store->invalidate("pch", pch_key);
    }
    // An in-flight rebuild owns the entry; its commit republishes fresh
    // blobs over the retracted pair, so only a settled entry is dropped.
    if(auto it = workspace.pch_cache.find(pch_key);
       it != workspace.pch_cache.end() && !building(pch_key)) {
        workspace.pch_cache.erase(it);
    }
}

void PCHFamily::blame(llvm::StringRef pch_key) {
    consume_blames.on_crash(pch_key);
    invalidate(pch_key);
}

}  // namespace clice

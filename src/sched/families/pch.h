#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "sched/crash_budget.h"
#include "sched/graph.h"
#include "project/project.h"
#include "worker/pool.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringMap.h"

namespace clice {

/// Preamble artifacts (a PCH and its pch.idx envelope, committed as one
/// pair) as a task-graph family: one node per content-addressed pch_key,
/// no edges, one round = one revalidation or build. The facade is the
/// only surface consumers touch.
///
/// The family owns the shared side of the policy: the pair-atomic store
/// commit and cache metadata (a round runs to a real reply, so no
/// cancellation can split blob from metadata), the shared-key crash
/// budget, and the cooperative response to advisory cancellation. The
/// adoption side stays with the server: which session points at the key
/// and per-document quarantine — crash evidence reaches it through the
/// dispatch owner's probe (see acquire).
class PCHFamily {
public:
    PCHFamily(TaskGraph& graph, Workspace& workspace, WorkerPool& pool);

    /// Register the production runner. Tests that drive the facade
    /// against a synthetic build register their own runner under
    /// Family::PCH instead.
    void register_runner();

    /// One acquisition's request conditions: the artifact identity plus
    /// the inputs a build needs if this acquire ends up dispatching one.
    struct Request {
        std::string pch_key;
        std::string file;
        std::string directory;
        std::vector<std::string> arguments;
        /// The requesting document's buffer text; the preamble is its
        /// prefix and the build sends the whole buffer.
        std::string content;
        std::uint32_t preamble_bound = 0;
    };

    enum class Outcome : std::uint8_t {
        /// A fresh pair is registered under the key (revalidated or just
        /// built) — adopt it.
        Ready,
        Failed,
        /// The observed attempt ended without a verdict (advisory
        /// cancellation, shutdown): no adoption, but no failure verdict
        /// either.
        Preempted,
    };

    /// Join the key's round, spawning one when none is live. The spawning
    /// acquire is the dispatch owner: its inputs feed the build, and its
    /// `on_crash` probe receives every worker death of the round at crash
    /// time — exactly once across all joiners, surviving the owner's own
    /// request going stale or unwinding (contract 12: a stale round's
    /// crashes still count). A non-spawning acquire's probe is simply
    /// never installed, so deaths are never replayed per joiner. One
    /// acquire observes exactly one attempt; retry policy stays with the
    /// caller.
    kota::task<Outcome> acquire(Request request, std::function<void(llvm::StringRef)> on_crash);

    /// The stash-only half of acquire, for waits that must be recorded as
    /// graph edges: intern the key, re-dirty a stale clean node, and
    /// install the dispatch owner's inputs when the caller's join will
    /// spawn the round. The caller then waits through RoundContext::depend
    /// on the returned node — the only wait form a family round may use.
    NodeId prepare(Request request, std::function<void(llvm::StringRef)> on_crash);

    /// A complete, store-backed, deps-current pair is registered under
    /// the key. Non-const: a passing deps check may repair the snapshot's
    /// stat fast path in place.
    bool fresh(llvm::StringRef pch_key);

    /// A round for the key is in flight; its commit will republish over
    /// concurrent store retractions.
    bool building(llvm::StringRef pch_key) const;

    /// Retract a pair the frontend could not consume: remove both blobs
    /// from the store and drop the settled cache entry, so the next
    /// acquire misses and rebuilds instead of trusting corrupt bytes for
    /// the life of the store.
    void invalidate(llvm::StringRef pch_key);

    /// A parse consuming the pair died or blamed it: retract it AND book a
    /// strike against the key. The build-side budget cannot bound this —
    /// each successful rebuild clears it — so consumption strikes keep
    /// their own ledger; enough of them park the key and acquisitions fail
    /// fast, so consumers fall back to compiling without a PCH instead of
    /// looping rebuild/blame forever over failing storage.
    void blame(llvm::StringRef pch_key);

    /// A parse consumed the key's pair and completed without blaming it:
    /// clear its consumption strikes (cf. CrashBudget::on_land).
    void consumed_ok(llvm::StringRef pch_key) {
        consume_blames.on_land(pch_key);
    }

    /// Open the pch.idx envelope of a cached PCH. The single consumption
    /// gate for `.pch.idx` blobs: when the blob turns out unreadable, the
    /// on-disk pair is retracted from the store as well — otherwise every
    /// later session re-adopts the corrupt pair from the artifacts blob and
    /// silently degrades again. With the pair gone the next ensure_pch is
    /// a miss and rebuilds both halves. Loads count against the
    /// loaded-state budget (see enforce_loaded_budget).
    std::shared_ptr<index::TUIndex> preamble_state(llvm::StringRef pch_key);

    /// Unload pch.idx envelopes beyond the budget (open documents + 2),
    /// least recently used first. Without this every preamble key ever
    /// touched keeps its blob mapped for the server's lifetime — tens of
    /// MB per key on real projects, released by neither didClose nor
    /// store eviction. Unloading only drops the entry's reference:
    /// consumers holding the shared_ptr finish safely, and the next use
    /// reopens the blob from disk.
    void enforce_loaded_budget();

    /// Open-document count provider, wired by the master. Sizes the
    /// loaded-state budget; unset (tests, tools) falls back to
    /// default_open_documents.
    std::function<std::size_t()> open_documents;

private:
    /// One PCH round: run one attempt and retire the stash once a current
    /// round lands a verdict.
    kota::task<RoundOutcome> run(RoundContext& ctx, std::uint64_t key_id);

    /// One attempt: revalidate the registered pair, or dispatch a build
    /// with the dispatch owner's inputs and commit the pair.
    kota::task<RoundOutcome> attempt(RoundContext& ctx, std::uint64_t key_id);

    std::uint64_t intern(llvm::StringRef pch_key);

    static NodeId node(std::uint64_t key_id) {
        return {Family::PCH, key_id};
    }

    /// Per-key slot for the dispatch owner's stash; both fields are
    /// replaced by the spawning acquire, read by its round and any
    /// stale-retry rounds, and retired when a current round lands a
    /// verdict.
    struct KeyState {
        Request inputs;
        std::function<void(llvm::StringRef)> on_crash;
    };

    TaskGraph& graph;
    Workspace& workspace;
    WorkerPool& pool;

    /// Move a pch key to the front of the loaded-state LRU. Called
    /// whenever an entry's envelope is opened or replaced.
    void touch_loaded_state(llvm::StringRef pch_key);

    /// Crash budget of the builds, keyed by the content-derived pch key:
    /// a preamble that keeps killing workers is refused until its content
    /// — and therefore its key — changes. Document quarantine cannot
    /// contain it: the artifact is shared, so every session with the same
    /// preamble would burn workers of its own.
    CrashBudget build_crashes;

    /// Consumption strikes per key (see blame); separate from the
    /// build-side build_crashes, which every successful rebuild clears.
    CrashBudget consume_blames;

    /// Keys of pch_cache entries whose envelope is currently loaded,
    /// most recently used first (see enforce_loaded_budget).
    llvm::SmallVector<std::string, 8> loaded_state_lru;

    llvm::StringMap<std::uint64_t> ids;
    std::vector<KeyState> states;
};

}  // namespace clice

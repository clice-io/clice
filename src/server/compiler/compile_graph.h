#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

#include "kota/async/async.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"

namespace clice {

struct CompileUnit {
    /// Result of one compilation round, observed by waiters after the
    /// round's completion event fires.
    enum class Outcome : std::uint8_t {
        /// The round was cancelled (file update or loss of interest) and its
        /// result discarded; waiters that still hold interest retry.
        Stale,
        Success,
        /// Dispatch failed or a dependency cycle was detected; waiters
        /// propagate the failure instead of retrying.
        Failed,
    };

    /// State of one compilation round. Waiters capture the shared_ptr before
    /// suspending so the completion event outlives both map mutations and
    /// the unit task that publishes the outcome.
    struct Round {
        kota::event completion;
        Outcome outcome = Outcome::Stale;
    };

    std::uint32_t path_id = 0;

    /// Dependencies discovered lazily by resolve_fn.
    llvm::SmallVector<std::uint32_t> dependencies;

    /// Back-edges: units that depend on this unit.
    llvm::SmallVector<std::uint32_t> dependents;

    /// Whether resolve_fn has been called for this unit.
    bool resolved = false;

    bool dirty = true;
    bool compiling = false;

    /// In-flight interest count: one per requesting root plus one per edge
    /// held by a dependent unit's running task. Dropping to zero while
    /// compiling cancels this unit's round. Not a lifetime count.
    std::uint32_t refcount = 0;

    /// Monotonic counter bumped by update(); detects results that became
    /// stale while the cancellation raced with a dispatch completion.
    std::uint64_t generation = 0;

    /// Cancellation scope of the current round; replaced after every cancel
    /// so the next round always starts with a fresh token.
    std::unique_ptr<kota::cancellation_source> source =
        std::make_unique<kota::cancellation_source>();

    /// Current (or most recent) compilation round.
    std::shared_ptr<Round> round;
};

/// Module compilation DAG with interest-counted cancellation.
///
/// Each dirty unit is compiled by an independent task spawned into the
/// graph's task group, cancellable only through its own round token —
/// requesters and dependent units merely wait on the round's completion
/// event. Lifecycle by phase:
///
/// - Request arrival: compile()/compile_deps() takes a root reference on the
///   requested unit(s) and waits. A dirty unit with no running round gets a
///   unit task spawned; the task acquires edge references on its direct
///   dependencies, waits for them, then dispatches its own compilation.
/// - Request cancel: the requester's frame unwinds and drops its root
///   reference. A unit whose interest reaches zero mid-compile has its round
///   cancelled; the exiting task drops its edge references, cascading the
///   cancellation through no-longer-needed dependencies. Shared dependencies
///   keep compiling as long as any other consumer holds interest.
/// - File update: update() marks the unit and its transitive dependents
///   dirty and cancels their in-flight rounds — the results are stale.
///   Interest is NOT touched; waiters observe the stale round and drive a
///   fresh one with the new content.
/// - Compile finish: the unit task publishes success/failure through its
///   round and wakes waiters; success clears dirty, failure (compile error,
///   dependency cycle) propagates to waiters without retry.
class CompileGraph {
public:
    /// Performs the actual compilation (e.g. produce PCM file).
    using dispatch_fn = std::function<kota::task<bool>(std::uint32_t path_id)>;

    /// Returns the dependency path_ids for a given path_id (called lazily on first compile).
    using resolve_fn = std::function<llvm::SmallVector<std::uint32_t>(std::uint32_t path_id)>;

    CompileGraph(kota::event_loop& loop, dispatch_fn dispatch, resolve_fn resolve);

    /// Compile a unit and all its transitive dependencies.
    kota::task<bool> compile(std::uint32_t path_id);

    /// Compile all transitive module dependencies of path_id, but NOT path_id itself.
    /// Used for non-module files (plain .cpp) that import modules.
    kota::task<bool> compile_deps(std::uint32_t path_id);

    /// Mark path_id and all transitive dependents as dirty,
    /// cancelling any in-progress compilations (their results are stale).
    /// Returns the set of all path_ids that were marked dirty.
    llvm::SmallVector<std::uint32_t> update(std::uint32_t path_id);

    /// Cancel every in-flight round regardless of interest. Waiters that
    /// still hold interest respawn their units afterwards.
    void cancel_all();

    /// Cancel all unit tasks and wait for their frames to unwind. Must be
    /// awaited exactly once before the graph is destroyed; no compilation
    /// can be started afterwards.
    kota::task<> shutdown();

    bool has_unit(std::uint32_t path_id) const;
    bool is_dirty(std::uint32_t path_id) const;
    bool is_compiling(std::uint32_t path_id) const;

    /// Current in-flight interest count for a unit (testing/diagnostics).
    std::uint32_t refcount(std::uint32_t path_id) const;

    /// All bookkeeping is quiesced: nothing compiling, no interest held and
    /// every round's completion has fired. Holds whenever no request is in
    /// flight and all unit tasks have unwound (e.g. after shutdown()).
    bool idle() const;

    /// Structural sanity that holds at every drain boundary: a compiling
    /// unit has an unfinished round, and a finished round never leaves the
    /// compiling flag behind.
    bool consistent() const;

private:
    struct RefGuard;
    struct UnitGuard;

    /// Get or create a unit, resolving its dependencies if needed.
    void ensure_resolved(std::uint32_t path_id);

    /// Interest +1; creates the unit if needed.
    void acquire(std::uint32_t path_id);

    /// Interest -1; cancels the unit's round when it drops to zero mid-compile.
    void release(std::uint32_t path_id);

    /// Cancel the current round and install a fresh cancellation scope.
    void cancel_round(CompileUnit& unit);

    /// Start a unit task for path_id in the graph's task group.
    /// Returns false when the graph is shutting down.
    bool spawn_unit(std::uint32_t path_id);

    /// One compilation round of a unit, cancelled only through its round token.
    kota::task<> unit_body(std::uint32_t path_id, std::shared_ptr<CompileUnit::Round> round);

    /// Spawned wrapper: runs unit_body under the round token and absorbs the
    /// cancellation outcome. The task group treats a cancelled child as a
    /// reason to abort every sibling (structured fail-fast); a round
    /// cancelled via its token is a normal event and must not do that.
    kota::task<> unit_task(std::uint32_t path_id,
                           std::shared_ptr<CompileUnit::Round> round,
                           kota::cancellation_token token);

    /// Wait until path_id reaches a terminal outcome, respawning its unit
    /// task whenever a stale round invalidates the previous attempt.
    /// `waiter` is the unit doing the waiting (for deadlock detection),
    /// or nullopt for requests.
    kota::task<bool> await_unit(std::uint32_t path_id, std::optional<std::uint32_t> waiter);

    /// Check if waiting on `target` would deadlock: walks the dependency
    /// graph through compiling units to see if any dependency transitively
    /// reaches the waiting unit.
    bool has_wait_cycle(std::uint32_t target, std::uint32_t waiter) const;

    dispatch_fn dispatch;
    resolve_fn resolve;
    llvm::DenseMap<std::uint32_t, CompileUnit> units;

    /// Owns every unit task; structured shutdown via shutdown().
    kota::task_group<> tasks;
};

}  // namespace clice

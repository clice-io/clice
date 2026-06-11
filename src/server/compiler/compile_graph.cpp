#include "server/compiler/compile_graph.h"

#include <algorithm>
#include <cassert>

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseSet.h"

namespace clice {

namespace ranges = std::ranges;

/// Request scope: holds root references for one compile()/compile_deps()
/// call. The destructor runs on every exit path, including cancellation
/// unwind of the requester's frame.
struct CompileGraph::RefGuard {
    CompileGraph& graph;
    llvm::SmallVector<std::uint32_t, 4> held;

    RefGuard(CompileGraph& graph, llvm::ArrayRef<std::uint32_t> ids) :
        graph(graph), held(ids.begin(), ids.end()) {
        for(auto id: held) {
            graph.acquire(id);
        }
    }

    RefGuard(const RefGuard&) = delete;
    RefGuard& operator=(const RefGuard&) = delete;

    ~RefGuard() {
        for(auto id: held) {
            graph.release(id);
        }
    }
};

/// Maintains all per-round invariants of a unit task. kotatsu cancellation
/// destroys a suspended frame without resuming it, so code after a co_await
/// never runs on the cancel path — only destructors of locals established
/// before the first suspension are guaranteed to execute. This guard is that
/// destructor; the body must not maintain unit state any other way.
struct CompileGraph::UnitGuard {
    CompileGraph& graph;
    std::uint32_t path_id;
    std::shared_ptr<CompileUnit::Round> round;
    CompileUnit::Outcome outcome = CompileUnit::Outcome::Stale;

    /// Edge references acquired by this task; registration here must stay
    /// synchronous with the matching refcount increment.
    llvm::SmallVector<std::uint32_t, 8> acquired;

    ~UnitGuard() {
        // Publish the outcome, clear the compiling flag, release edge
        // references, then wake waiters — all synchronous. Resumes triggered
        // by cancel()/set() are deferred by the event loop, so nothing
        // re-enters the graph mid-destructor.
        round->outcome = outcome;

        auto& unit = graph.units.find(path_id)->second;
        assert(unit.compiling && unit.round == round && "unit round bookkeeping out of sync");
        unit.compiling = false;

        for(auto dep_id: acquired) {
            graph.release(dep_id);
        }

        round->completion.set();
    }
};

CompileGraph::CompileGraph(kota::event_loop& loop, dispatch_fn dispatch, resolve_fn resolve) :
    dispatch(std::move(dispatch)), resolve(std::move(resolve)), tasks(loop) {}

void CompileGraph::ensure_resolved(std::uint32_t path_id) {
    auto& unit = units[path_id];
    if(unit.resolved) {
        return;
    }

    unit.path_id = path_id;
    unit.resolved = true;
    unit.dependencies = resolve(path_id);

    // Copy deps locally — the loop below may insert into `units`,
    // which can rehash the DenseMap and invalidate the `unit` reference.
    auto deps = units[path_id].dependencies;

    // Back-populate dependents.
    for(auto dep_id: deps) {
        auto& dep = units[dep_id];
        dep.path_id = dep_id;
        dep.dependents.push_back(path_id);
    }
}

void CompileGraph::acquire(std::uint32_t path_id) {
    auto& unit = units[path_id];
    unit.path_id = path_id;
    unit.refcount += 1;
}

void CompileGraph::release(std::uint32_t path_id) {
    auto& unit = units.find(path_id)->second;
    assert(unit.refcount > 0 && "released more interest than acquired");
    unit.refcount -= 1;

    // No interest left in an in-flight round: cancel it. The task unwinds
    // asynchronously and its guard finishes the bookkeeping, releasing its
    // own edge references in turn (cascading the cancellation).
    if(unit.refcount == 0 && unit.compiling) {
        cancel_round(unit);
    }
}

void CompileGraph::cancel_round(CompileUnit& unit) {
    unit.source->cancel();
    unit.source = std::make_unique<kota::cancellation_source>();
}

bool CompileGraph::spawn_unit(std::uint32_t path_id) {
    auto& unit = units.find(path_id)->second;
    assert(!unit.compiling && "spawn requested while a round is in flight");
    unit.compiling = true;
    unit.round = std::make_shared<CompileUnit::Round>();
    auto round = unit.round;
    auto token = unit.source->token();

    // spawn resumes the body synchronously up to its first suspension point,
    // which may insert units and invalidate `unit` — don't touch it below.
    if(tasks.spawn(unit_task(path_id, round, token))) {
        return true;
    }

    // The graph is shutting down: roll back so concurrent waiters observe a
    // stale, completed round instead of hanging.
    units.find(path_id)->second.compiling = false;
    round->completion.set();
    return false;
}

kota::task<> CompileGraph::unit_task(std::uint32_t path_id,
                                     std::shared_ptr<CompileUnit::Round> round,
                                     kota::cancellation_token token) {
    // The cancellation surfaces here as an explicit outcome instead of
    // unwinding this wrapper, so the task always completes as Finished.
    co_await kota::with_token(unit_body(path_id, std::move(round)), std::move(token));
}

kota::task<> CompileGraph::unit_body(std::uint32_t path_id,
                                     std::shared_ptr<CompileUnit::Round> round) {
    UnitGuard guard{*this, path_id, std::move(round)};

    ensure_resolved(path_id);

    auto& unit = units.find(path_id)->second;
    auto gen = unit.generation;
    // Copy deps — the map may rehash while this frame is suspended.
    auto deps = unit.dependencies;

    // Trivial cycle: a unit depending on itself can never make progress.
    if(ranges::contains(deps, path_id)) {
        guard.outcome = CompileUnit::Outcome::Failed;
        co_return;
    }

    // Acquire edge references on all direct dependencies. This must stay
    // synchronous: no suspension between refcount++ and guard registration.
    for(auto dep_id: deps) {
        acquire(dep_id);
        guard.acquired.push_back(dep_id);
    }

    if(!deps.empty()) {
        std::vector<kota::task<bool>> waits;
        waits.reserve(deps.size());
        for(auto dep_id: deps) {
            waits.push_back(await_unit(dep_id, path_id));
        }

        auto results = co_await kota::when_all(std::move(waits));
        if(!ranges::all_of(results, [](bool ok) { return ok; })) {
            guard.outcome = CompileUnit::Outcome::Failed;
            co_return;
        }
    }

    bool ok = co_await dispatch(path_id);

    // Synchronous tail: nothing can interleave between dispatch resuming us
    // and co_return, so the checks below are atomic.
    if(!ok) {
        guard.outcome = CompileUnit::Outcome::Failed;
        co_return;
    }

    auto& fresh = units.find(path_id)->second;
    if(fresh.generation != gen) {
        // update() raced with dispatch completion: our cancellation was
        // signalled but this frame resumed first. The result is stale.
        co_return;
    }

    fresh.dirty = false;
    guard.outcome = CompileUnit::Outcome::Success;
}

kota::task<bool> CompileGraph::await_unit(std::uint32_t path_id,
                                          std::optional<std::uint32_t> waiter) {
    while(true) {
        auto& unit = units.find(path_id)->second;
        if(!unit.dirty) {
            co_return true;
        }

        if(!unit.compiling && !spawn_unit(path_id)) {
            co_return false;  // graph is shutting down
        }

        // Re-find: spawn_unit runs the unit body synchronously, which may
        // rehash the map (and may even complete the round outright).
        auto round = units.find(path_id)->second.round;

        // Blocking on a unit whose dependency chain reaches back to the
        // waiting unit would deadlock — fail as a dependency cycle instead.
        if(!round->completion.is_set() && waiter && has_wait_cycle(path_id, *waiter)) {
            co_return false;
        }

        co_await round->completion.wait();

        switch(round->outcome) {
            case CompileUnit::Outcome::Success: co_return true;
            case CompileUnit::Outcome::Failed: co_return false;
            // The round was cancelled and produced no result; we still hold
            // interest, so drive a new round. Each retry consumes one
            // staleness event — without further updates this terminates.
            case CompileUnit::Outcome::Stale: break;
        }
    }
}

kota::task<bool> CompileGraph::compile(std::uint32_t path_id) {
    // Request scope: one root reference, dropped when the requester exits or
    // its frame is cancelled.
    RefGuard scope(*this, {path_id});
    co_return co_await await_unit(path_id, std::nullopt);
}

kota::task<bool> CompileGraph::compile_deps(std::uint32_t path_id) {
    ensure_resolved(path_id);

    // Copy deps — the map may rehash while this frame is suspended.
    auto deps = units.find(path_id)->second.dependencies;
    if(deps.empty()) {
        co_return true;
    }

    // Request scope: root references on each direct dependency (path_id
    // itself is never dispatched here).
    RefGuard scope(*this, deps);

    std::vector<kota::task<bool>> waits;
    waits.reserve(deps.size());
    for(auto dep_id: deps) {
        waits.push_back(await_unit(dep_id, std::nullopt));
    }

    auto results = co_await kota::when_all(std::move(waits));
    co_return ranges::all_of(results, [](bool ok) { return ok; });
}

llvm::SmallVector<std::uint32_t> CompileGraph::update(std::uint32_t path_id) {
    llvm::SmallVector<std::uint32_t> queue;
    llvm::SmallVector<std::uint32_t> dirtied;
    queue.push_back(path_id);

    // Track visited nodes to avoid processing the same node twice.
    llvm::DenseSet<std::uint32_t> visited;

    while(!queue.empty()) {
        auto current = queue.pop_back_val();

        if(!visited.insert(current).second) {
            continue;
        }

        auto it = units.find(current);
        if(it == units.end()) {
            continue;
        }

        auto& unit = it->second;

        // Reset resolved so dependencies are re-scanned on next compile
        // (the source file may have added/removed imports).
        if(current == path_id) {
            unit.resolved = false;
            // Clear stale dependency edges — they'll be rebuilt by ensure_resolved.
            for(auto dep_id: unit.dependencies) {
                auto dep_it = units.find(dep_id);
                if(dep_it != units.end()) {
                    auto& dependents = dep_it->second.dependents;
                    dependents.erase(ranges::remove(dependents, path_id).begin(), dependents.end());
                }
            }
            unit.dependencies.clear();
        }

        // The in-flight result (if any) is stale: cancel the round. Interest
        // counts are untouched — waiters keep their references and drive a
        // fresh round once the cancelled task unwinds.
        cancel_round(unit);
        unit.dirty = true;
        unit.generation++;
        dirtied.push_back(current);

        // Always propagate to dependents.
        for(auto dep_id: unit.dependents) {
            queue.push_back(dep_id);
        }
    }

    return dirtied;
}

bool CompileGraph::has_wait_cycle(std::uint32_t target, std::uint32_t waiter) const {
    // BFS through the target's dependency chain, following only compiling
    // units. If any dependency reaches the waiting unit, waiting would deadlock.
    llvm::SmallVector<std::uint32_t> queue;
    llvm::DenseSet<std::uint32_t> visited;
    queue.push_back(target);

    while(!queue.empty()) {
        auto current = queue.pop_back_val();
        if(!visited.insert(current).second) {
            continue;
        }
        auto it = units.find(current);
        if(it == units.end()) {
            continue;
        }
        for(auto dep_id: it->second.dependencies) {
            if(dep_id == waiter) {
                return true;
            }
            auto dep_it = units.find(dep_id);
            if(dep_it != units.end() && dep_it->second.compiling) {
                queue.push_back(dep_id);
            }
        }
    }
    return false;
}

void CompileGraph::cancel_all() {
    for(auto& [_, unit]: units) {
        cancel_round(unit);
    }
}

kota::task<> CompileGraph::shutdown() {
    // Structured two-step shutdown: cancel every unit task regardless of
    // interest, then wait for the frames to unwind. The task group must be
    // joined before destruction.
    tasks.cancel();
    co_await tasks.join();
}

bool CompileGraph::has_unit(std::uint32_t path_id) const {
    return units.count(path_id);
}

bool CompileGraph::is_dirty(std::uint32_t path_id) const {
    auto it = units.find(path_id);
    return it != units.end() && it->second.dirty;
}

bool CompileGraph::is_compiling(std::uint32_t path_id) const {
    auto it = units.find(path_id);
    return it != units.end() && it->second.compiling;
}

std::uint32_t CompileGraph::refcount(std::uint32_t path_id) const {
    auto it = units.find(path_id);
    return it != units.end() ? it->second.refcount : 0;
}

bool CompileGraph::idle() const {
    return ranges::all_of(units, [](const auto& entry) {
        const auto& unit = entry.second;
        bool round_done = !unit.round || unit.round->completion.is_set();
        return !unit.compiling && unit.refcount == 0 && round_done;
    });
}

bool CompileGraph::consistent() const {
    return ranges::all_of(units, [](const auto& entry) {
        const auto& unit = entry.second;
        return !unit.compiling || (unit.round && !unit.round->completion.is_set());
    });
}

}  // namespace clice

#include "server/compile_graph.h"

#include <algorithm>

#include "llvm/ADT/DenseSet.h"

namespace clice {

namespace ranges = std::ranges;

CompileGraph::CompileGraph(dispatch_fn dispatch, resolve_fn resolve) :
    dispatch(std::move(dispatch)), resolve(std::move(resolve)) {}

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

kota::task<bool> CompileGraph::compile_deps(std::uint32_t path_id) {
    llvm::DenseSet<std::uint32_t> ancestors;
    co_return co_await compile_impl(path_id, ancestors, false);
}

kota::task<bool> CompileGraph::compile(std::uint32_t path_id) {
    llvm::DenseSet<std::uint32_t> ancestors;
    co_return co_await compile_impl(path_id, ancestors);
}

kota::task<bool> CompileGraph::compile_impl(std::uint32_t path_id,
                                            llvm::DenseSet<std::uint32_t> ancestors,
                                            bool dispatch_self) {
    ensure_resolved(path_id);

    // Cycle detection: if this unit is already in the compile chain, bail out.
    if(!ancestors.insert(path_id).second) {
        co_return false;
    }

    // Re-lookup after ensure_resolved may have mutated the map.
    auto it = units.find(path_id);

    // For deps-only mode, compile dependencies concurrently and return.
    if(!dispatch_self) {
        auto deps = it->second.dependencies;
        if(deps.empty()) {
            co_return true;
        }

        std::vector<kota::task<bool>> dep_tasks;
        dep_tasks.reserve(deps.size());
        for(auto dep_id: deps) {
            dep_tasks.push_back(compile_impl(dep_id, ancestors));
        }
        auto results = co_await kota::when_all(std::move(dep_tasks));
        for(auto ok: results) {
            if(!ok) {
                co_return false;
            }
        }
        co_return true;
    }

    // Already clean.
    if(!it->second.dirty) {
        co_return true;
    }

    // Another task is already compiling this unit — wait for it,
    // but first check that waiting won't deadlock (cross-branch cycle).
    if(it->second.compiling) {
        if(has_wait_cycle(path_id, ancestors)) {
            co_return false;
        }
        auto& completion = *it->second.completion;
        co_await completion.wait();
        co_return !units.find(path_id)->second.dirty;
    }

    // Begin compilation. The finish lambda ensures compiling/completion state
    // is always cleaned up, regardless of how the function exits.
    it->second.compiling = true;
    it->second.completion = std::make_unique<kota::event>();

    auto finish = [&, path_id] {
        auto& u = units.find(path_id)->second;
        u.compiling = false;
        u.completion->set();
    };

    // Copy deps and capture generation before co_await (DenseMap iterator safety).
    auto deps = it->second.dependencies;
    auto gen = it->second.generation;
    auto token = it->second.source->token();

    // Compile all dependencies concurrently.
    // Deadlocks from cross-branch cycles (e.g. 1->{2,3}, 2->3, 3->2) are
    // prevented by has_wait_cycle() checking before completion.wait().
    if(!deps.empty()) {
        std::vector<kota::task<bool, void, kota::cancellation>> dep_tasks;
        dep_tasks.reserve(deps.size());
        for(auto dep_id: deps) {
            dep_tasks.push_back(kota::with_token(compile_impl(dep_id, ancestors), token));
        }

        auto results = co_await kota::when_all(std::move(dep_tasks));

        if(results.is_cancelled()) {
            finish();
            co_await kota::cancel();
        }

        for(auto ok: *results) {
            if(!ok) {
                finish();
                co_return false;
            }
        }
    }

    // Dispatch the actual compilation, cancellable via the pre-captured token.
    auto result = co_await kota::with_token(dispatch(path_id), token);

    if(!result.has_value()) {
        finish();
        co_await kota::cancel();
    }

    if(!*result) {
        finish();
        co_return false;
    }

    // Success — only clear dirty if update() hasn't bumped the generation.
    auto& final_unit = units.find(path_id)->second;
    if(final_unit.generation != gen) {
        finish();
        co_return false;
    }

    final_unit.dirty = false;
    finish();
    co_return true;
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

        // Cancel in-flight compilation if running.
        if(unit.compiling) {
            unit.source->cancel();
            unit.source = std::make_unique<kota::cancellation_source>();
        }
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

bool CompileGraph::has_wait_cycle(std::uint32_t target,
                                  const llvm::DenseSet<std::uint32_t>& ancestors) const {
    // BFS through the target's dependency chain, following only compiling units.
    // If any dependency is in our ancestor chain, waiting would deadlock.
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
            if(ancestors.count(dep_id)) {
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
        unit.source->cancel();
        unit.source = std::make_unique<kota::cancellation_source>();
    }
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

}  // namespace clice

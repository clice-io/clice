#include "server/compile_graph.h"

#include <algorithm>

#include "llvm/ADT/DenseSet.h"

namespace clice {

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

et::task<bool> CompileGraph::compile(std::uint32_t path_id) {
    llvm::DenseSet<std::uint32_t> ancestors;
    co_return co_await compile_impl(path_id, ancestors);
}

et::task<bool> CompileGraph::compile_impl(std::uint32_t path_id,
                                          llvm::DenseSet<std::uint32_t> ancestors) {
    ensure_resolved(path_id);

    // Cycle detection: if this unit is already in the compile chain, bail out.
    if(!ancestors.insert(path_id).second) {
        co_return false;
    }

    // Re-lookup after ensure_resolved may have mutated the map.
    auto it = units.find(path_id);

    // Already clean.
    if(!it->second.dirty) {
        co_return true;
    }

    // Another task is already compiling this unit — wait for it.
    if(it->second.compiling) {
        auto& completion = *it->second.completion;
        co_await completion.wait();
        co_return !units.find(path_id)->second.dirty;
    }

    // Begin compilation.
    it->second.compiling = true;
    it->second.completion = std::make_unique<et::event>();

    // Copy deps and token before co_await (DenseMap iterator safety).
    auto deps = it->second.dependencies;
    auto token = it->second.source->token();

    // Compile all dependencies in parallel.
    if(!deps.empty()) {
        std::vector<et::task<bool, void, et::cancellation>> dep_tasks;
        dep_tasks.reserve(deps.size());
        for(auto dep_id: deps) {
            dep_tasks.push_back(et::with_token(compile_impl(dep_id, ancestors), token));
        }

        auto results = co_await et::when_all(std::move(dep_tasks));

        auto& u = units.find(path_id)->second;
        if(results.is_cancelled()) {
            u.compiling = false;
            u.completion->set();
            co_await et::cancel();
        }

        for(auto ok: *results) {
            if(!ok) {
                u.compiling = false;
                u.completion->set();
                co_return false;
            }
        }
    }

    // Dispatch the actual compilation, cancellable via this unit's token.
    {
        auto result =
            co_await et::with_token(dispatch(path_id), units.find(path_id)->second.source->token());

        auto& u = units.find(path_id)->second;
        if(!result.has_value()) {
            u.compiling = false;
            u.completion->set();
            co_await et::cancel();
        }
        if(!*result) {
            u.compiling = false;
            u.completion->set();
            co_return false;
        }
    }

    // Success.
    auto& final_unit = units.find(path_id)->second;
    final_unit.dirty = false;
    final_unit.compiling = false;
    final_unit.completion->set();
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
                    dependents.erase(std::remove(dependents.begin(), dependents.end(), path_id),
                                     dependents.end());
                }
            }
            unit.dependencies.clear();
        }

        // Cancel in-flight compilation if running.
        if(unit.compiling) {
            unit.source->cancel();
            unit.source = std::make_unique<et::cancellation_source>();
        }
        unit.dirty = true;
        dirtied.push_back(current);

        // Always propagate to dependents.
        for(auto dep_id: unit.dependents) {
            queue.push_back(dep_id);
        }
    }

    return dirtied;
}

void CompileGraph::cancel_all() {
    for(auto& [_, unit]: units) {
        unit.source->cancel();
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

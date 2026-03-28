#include "server/compile_graph.h"

namespace clice {

CompileGraph::CompileGraph(et::event_loop& loop, dispatch_fn dispatch) :
    loop(loop), dispatch(std::move(dispatch)) {}

void CompileGraph::register_unit(std::uint32_t path_id, llvm::ArrayRef<std::uint32_t> deps) {
    auto& unit = units[path_id];
    unit.path_id = path_id;
    unit.dependencies.assign(deps.begin(), deps.end());

    for(auto dep_id: deps) {
        auto& dep_unit = units[dep_id];
        dep_unit.path_id = dep_id;
        dep_unit.dependents.push_back(path_id);
    }
}

et::task<bool> CompileGraph::compile(std::uint32_t path_id) {
    auto it = units.find(path_id);
    if(it == units.end()) {
        co_return true;
    }

    // Compile each dependency sequentially (dedup is handled by compile_impl).
    for(auto dep_id: it->second.dependencies) {
        auto result = co_await et::with_token(compile_impl(dep_id), units[path_id].source->token());
        if(!result.has_value()) {
            co_return false;  // Cancelled.
        }
        if(!*result) {
            co_return false;  // Dep failed.
        }
    }

    co_return true;
}

void CompileGraph::update(std::uint32_t path_id) {
    llvm::SmallVector<std::uint32_t> queue;
    queue.push_back(path_id);

    while(!queue.empty()) {
        auto current = queue.pop_back_val();

        auto it = units.find(current);
        if(it == units.end()) {
            continue;
        }

        auto& unit = it->second;

        // Skip if already dirty and not compiling (no work to do).
        if(unit.dirty && !unit.compiling) {
            continue;
        }

        // Cancel any in-progress compilation and create a fresh source.
        unit.source->cancel();
        unit.source = std::make_unique<et::cancellation_source>();
        unit.dirty = true;

        // Cascade to all dependents.
        for(auto dep_id: unit.dependents) {
            queue.push_back(dep_id);
        }
    }
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

et::task<bool> CompileGraph::compile_impl(std::uint32_t path_id,
                                          llvm::SmallVector<std::uint32_t>* stack) {
    // Cycle detection.
    if(stack) {
        if(llvm::find(*stack, path_id) != stack->end()) {
            co_return false;
        }
        stack->push_back(path_id);
    }

    auto it = units.find(path_id);
    if(it == units.end()) {
        if(stack) {
            stack->pop_back();
        }
        co_return false;
    }

    auto& unit = it->second;

    // Already clean.
    if(!unit.dirty) {
        if(stack) {
            stack->pop_back();
        }
        co_return true;
    }

    // Another task is already compiling this unit — wait for it.
    if(unit.compiling) {
        co_await unit.completion->wait();
        auto& u = units.find(path_id)->second;
        if(stack) {
            stack->pop_back();
        }
        co_return !u.dirty;
    }

    // Begin compilation.
    unit.compiling = true;
    unit.completion = std::make_unique<et::event>();

    // Compile dependencies sequentially, each cancellable via this unit's token.
    for(auto dep_id: units.find(path_id)->second.dependencies) {
        auto dep_result = co_await et::with_token(compile_impl(dep_id, stack),
                                                  units.find(path_id)->second.source->token());
        if(!dep_result.has_value()) {
            // Cancelled — cleanup and propagate.
            auto& u = units.find(path_id)->second;
            u.compiling = false;
            u.completion->set();
            if(stack) {
                stack->pop_back();
            }
            co_await et::cancel();
            co_return false;  // Unreachable.
        }
        if(!*dep_result) {
            // Dependency failed.
            auto& u = units.find(path_id)->second;
            u.compiling = false;
            u.completion->set();
            if(stack) {
                stack->pop_back();
            }
            co_return false;
        }
    }

    // All dependencies ready — dispatch the actual compilation.
    {
        auto& cur = units.find(path_id)->second;
        auto result = co_await et::with_token(dispatch(path_id), cur.source->token());
        if(!result.has_value()) {
            // Cancelled.
            auto& u = units.find(path_id)->second;
            u.compiling = false;
            u.completion->set();
            if(stack) {
                stack->pop_back();
            }
            co_await et::cancel();
            co_return false;  // Unreachable.
        }
        if(!*result) {
            // Dispatch returned false (compilation failed).
            auto& u = units.find(path_id)->second;
            u.compiling = false;
            u.completion->set();
            if(stack) {
                stack->pop_back();
            }
            co_return false;
        }
    }

    // Success.
    auto& final_unit = units.find(path_id)->second;
    final_unit.dirty = false;
    final_unit.compiling = false;
    final_unit.completion->set();
    if(stack) {
        stack->pop_back();
    }
    co_return true;
}

}  // namespace clice

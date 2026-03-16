#include "server/compile_graph.h"

#include "support/logging.h"

namespace clice {

namespace et = eventide;

void CompileGraph::register_unit(std::uint32_t path_id,
                                 llvm::ArrayRef<std::uint32_t> deps) {
    auto& unit = units[path_id];
    unit.path_id = path_id;

    for(auto old_dep : unit.dependencies) {
        auto dep_it = units.find(old_dep);
        if(dep_it != units.end()) {
            auto& dependents = dep_it->second.dependents;
            dependents.erase(
                std::remove(dependents.begin(), dependents.end(), path_id),
                dependents.end());
        }
    }

    unit.dependencies.assign(deps.begin(), deps.end());

    for(auto dep_id : deps) {
        auto& dep_unit = units[dep_id];
        dep_unit.path_id = dep_id;
        dep_unit.dependents.push_back(path_id);
    }
}

void CompileGraph::remove_unit(std::uint32_t path_id) {
    auto it = units.find(path_id);
    if(it == units.end())
        return;

    for(auto dep_id : it->second.dependencies) {
        auto dep_it = units.find(dep_id);
        if(dep_it != units.end()) {
            auto& dependents = dep_it->second.dependents;
            dependents.erase(
                std::remove(dependents.begin(), dependents.end(), path_id),
                dependents.end());
        }
    }

    for(auto dep_id : it->second.dependents) {
        auto dep_it = units.find(dep_id);
        if(dep_it != units.end()) {
            auto& dependencies = dep_it->second.dependencies;
            dependencies.erase(
                std::remove(dependencies.begin(), dependencies.end(), path_id),
                dependencies.end());
        }
    }

    units.erase(it);
    pch_cache.erase(path_id);
    pcm_cache.erase(path_id);
}

void CompileGraph::update(std::uint32_t path_id) {
    llvm::SmallVector<std::uint32_t> queue;
    queue.push_back(path_id);

    while(!queue.empty()) {
        auto current = queue.pop_back_val();

        auto it = units.find(current);
        if(it == units.end())
            continue;

        auto& unit = it->second;
        if(unit.dirty && !unit.compiling)
            continue;

        unit.source->cancel();
        unit.source = std::make_unique<et::cancellation_source>();
        unit.dirty = true;

        for(auto dep_id : unit.dependents) {
            queue.push_back(dep_id);
        }
    }
}

et::task<bool> CompileGraph::compile_deps(std::uint32_t path_id,
                                           et::event_loop& loop) {
    auto it = units.find(path_id);
    if(it == units.end())
        co_return true;

    auto& unit = it->second;

    for(auto dep_id : unit.dependencies) {
        auto dep_it = units.find(dep_id);
        if(dep_it == units.end())
            continue;
        if(dep_it->second.dirty && !dep_it->second.compiling) {
            loop.schedule(compile_impl(dep_id, loop));
        }
    }

    for(auto dep_id : unit.dependencies) {
        auto dep_it = units.find(dep_id);
        if(dep_it == units.end())
            continue;
        if(dep_it->second.compiling && dep_it->second.completion) {
            co_await dep_it->second.completion->wait();
        }
        dep_it = units.find(dep_id);
        if(dep_it != units.end() && dep_it->second.dirty) {
            co_return false;
        }
    }

    co_return true;
}

et::task<bool> CompileGraph::compile_impl(std::uint32_t path_id,
                                           et::event_loop& loop,
                                           llvm::SmallVector<std::uint32_t>* stack) {
    if(stack) {
        if(llvm::find(*stack, path_id) != stack->end()) {
            LOG_WARN("Circular dependency detected for path_id {}", path_id);
            co_return false;
        }
        stack->push_back(path_id);
    }

    auto it = units.find(path_id);
    if(it == units.end()) {
        if(stack)
            stack->pop_back();
        co_return false;
    }

    auto& unit = it->second;

    if(!unit.dirty) {
        if(stack)
            stack->pop_back();
        co_return true;
    }

    if(unit.compiling) {
        if(unit.completion) {
            co_await unit.completion->wait();
        }
        auto& u = units.find(path_id)->second;
        if(stack)
            stack->pop_back();
        co_return !u.dirty;
    }

    unit.compiling = true;
    unit.completion = std::make_unique<et::event>();

    for(auto dep_id : unit.dependencies) {
        auto dep_it = units.find(dep_id);
        if(dep_it == units.end())
            continue;
        if(dep_it->second.dirty && !dep_it->second.compiling) {
            loop.schedule(compile_impl(dep_id, loop, stack));
        }
    }

    for(auto dep_id : units.find(path_id)->second.dependencies) {
        auto dep_it = units.find(dep_id);
        if(dep_it == units.end())
            continue;
        if(dep_it->second.compiling && dep_it->second.completion) {
            co_await dep_it->second.completion->wait();
        }
        dep_it = units.find(dep_id);
        if(dep_it != units.end() && dep_it->second.dirty) {
            auto& u = units.find(path_id)->second;
            u.compiling = false;
            if(u.completion)
                u.completion->set();
            if(stack)
                stack->pop_back();
            co_return false;
        }
    }

    if(dispatcher) {
        auto& cur = units.find(path_id)->second;
        auto token = cur.source->token();
        bool success = co_await dispatcher(path_id, token);
        if(!success) {
            auto& u = units.find(path_id)->second;
            u.compiling = false;
            if(u.completion)
                u.completion->set();
            if(stack)
                stack->pop_back();
            co_return false;
        }
    }

    auto& final_unit = units.find(path_id)->second;
    final_unit.dirty = false;
    final_unit.compiling = false;
    if(final_unit.completion)
        final_unit.completion->set();
    if(stack)
        stack->pop_back();
    co_return true;
}

void CompileGraph::set_dispatcher(CompileDispatcher dispatcher) {
    this->dispatcher = std::move(dispatcher);
}

bool CompileGraph::has_unit(std::uint32_t path_id) const {
    return units.count(path_id) > 0;
}

const CompileUnit* CompileGraph::find_unit(std::uint32_t path_id) const {
    auto it = units.find(path_id);
    if(it == units.end())
        return nullptr;
    return &it->second;
}

std::pair<std::string, std::uint32_t>
CompileGraph::get_pch(std::uint32_t path_id) const {
    auto it = pch_cache.find(path_id);
    if(it == pch_cache.end())
        return {};
    return it->second;
}

llvm::StringMap<std::string>
CompileGraph::get_pcms(std::uint32_t path_id) const {
    llvm::StringMap<std::string> result;
    auto it = units.find(path_id);
    if(it == units.end())
        return result;

    for(auto dep_id : it->second.dependencies) {
        auto pcm_it = pcm_cache.find(dep_id);
        if(pcm_it != pcm_cache.end()) {
            result.try_emplace(pcm_it->second.first, pcm_it->second.second);
        }
    }
    return result;
}

void CompileGraph::set_pch_path(std::uint32_t path_id,
                                const std::string& pch_path,
                                std::uint32_t preamble_bound) {
    pch_cache[path_id] = {pch_path, preamble_bound};
}

void CompileGraph::set_pcm_path(std::uint32_t path_id,
                                const std::string& module_name,
                                const std::string& pcm_path) {
    pcm_cache[path_id] = {module_name, pcm_path};
}

}  // namespace clice

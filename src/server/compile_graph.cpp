#include "server/compile_graph.h"

#include "spdlog/spdlog.h"

namespace clice {

void CompileGraph::register_unit(std::uint32_t path_id, llvm::ArrayRef<std::uint32_t> deps) {
    auto& unit = units[path_id];
    unit.path_id = path_id;
    unit.dependencies.assign(deps.begin(), deps.end());
    for(auto dep_id: deps) {
        units[dep_id].dependents.push_back(path_id);
    }
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

        // Cancel ongoing compilation.
        unit.source->cancel();
        unit.source = std::make_unique<et::cancellation_source>();
        unit.dirty = true;

        // Cascade to dependents.
        for(auto dep_id: unit.dependents) {
            queue.push_back(dep_id);
        }
    }
}

et::task<bool> CompileGraph::compile_deps(std::uint32_t path_id, et::event_loop& loop) {
    auto it = units.find(path_id);
    if(it == units.end())
        co_return true;

    auto& unit = it->second;

    // Start compiling any dirty dependencies.
    for(auto dep_id: unit.dependencies) {
        auto dep_it = units.find(dep_id);
        if(dep_it == units.end())
            continue;
        if(dep_it->second.dirty && !dep_it->second.compiling) {
            loop.schedule(compile_impl(dep_id, loop));
        }
    }

    // Wait for all dependencies to complete.
    for(auto dep_id: unit.dependencies) {
        auto dep_it = units.find(dep_id);
        if(dep_it == units.end())
            continue;
        if(dep_it->second.compiling && dep_it->second.completion) {
            co_await dep_it->second.completion->wait();
        }
        // Re-lookup after co_await since the map may have been modified.
        dep_it = units.find(dep_id);
        if(dep_it != units.end() && dep_it->second.dirty) {
            co_return false;
        }
    }

    co_return true;
}

et::task<> CompileGraph::compile_impl(std::uint32_t path_id, et::event_loop& loop) {
    auto it = units.find(path_id);
    if(it == units.end())
        co_return;

    auto& unit = it->second;

    // Already clean.
    if(!unit.dirty)
        co_return;

    // Already being compiled -- wait for it.
    if(unit.compiling) {
        if(unit.completion) {
            co_await unit.completion->wait();
        }
        co_return;
    }

    // Start compilation.
    unit.compiling = true;
    unit.completion = std::make_unique<et::event>();

    // First compile dependencies recursively.
    for(auto dep_id: unit.dependencies) {
        auto dep_it = units.find(dep_id);
        if(dep_it == units.end())
            continue;
        if(dep_it->second.dirty && !dep_it->second.compiling) {
            loop.schedule(compile_impl(dep_id, loop));
        }
    }

    // Wait for dependencies, respecting cancellation.
    bool deps_ok = true;
    for(auto dep_id: units.find(path_id)->second.dependencies) {
        auto dep_it = units.find(dep_id);
        if(dep_it == units.end())
            continue;
        if(dep_it->second.compiling && dep_it->second.completion) {
            auto wait_result = co_await et::with_token(dep_it->second.completion->wait(),
                                                       units.find(path_id)->second.source->token())
                                   .catch_cancel();
            if(wait_result.is_cancelled()) {
                auto& u = units.find(path_id)->second;
                u.compiling = false;
                u.completion->set();
                co_return;
            }
        }
        // Check if dep is still dirty after wait.
        auto dep_it2 = units.find(dep_id);
        if(dep_it2 != units.end() && dep_it2->second.dirty) {
            deps_ok = false;
            break;
        }
    }

    if(!deps_ok) {
        auto& u = units.find(path_id)->second;
        u.compiling = false;
        u.completion->set();
        co_return;
    }

    // TODO: Dispatch actual PCH/PCM build to WorkerPool here.
    // The real compilation dispatch (BuildPCH/BuildPCM requests) will be
    // wired up when the MasterServer integration is complete. For now,
    // mark the unit as clean once dependencies are satisfied.
    spdlog::info("compile_graph: unit {} dependencies satisfied", path_id);

    auto& final_unit = units.find(path_id)->second;
    final_unit.dirty = false;
    final_unit.compiling = false;
    final_unit.completion->set();
}

void CompileGraph::cancel_all() {
    for(auto& [id, unit]: units) {
        if(unit.compiling) {
            unit.source->cancel();
            unit.source = std::make_unique<et::cancellation_source>();
        }
    }
}

}  // namespace clice

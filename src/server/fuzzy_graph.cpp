#include "server/fuzzy_graph.h"

#include "llvm/ADT/ArrayRef.h"

namespace clice {

void FuzzyGraph::update_file(std::uint32_t path_id,
                             llvm::ArrayRef<std::uint32_t> new_includes) {
    llvm::DenseSet<std::uint32_t> new_set(new_includes.begin(), new_includes.end());

    auto& old_set = forward[path_id];

    for(auto inc : old_set) {
        if(!new_set.count(inc)) {
            auto it = backward.find(inc);
            if(it != backward.end()) {
                it->second.erase(path_id);
                if(it->second.empty())
                    backward.erase(it);
            }
        }
    }

    for(auto inc : new_set) {
        if(!old_set.count(inc)) {
            backward[inc].insert(path_id);
        }
    }

    old_set = std::move(new_set);
}

void FuzzyGraph::remove_file(std::uint32_t path_id) {
    auto it = forward.find(path_id);
    if(it != forward.end()) {
        for(auto inc : it->second) {
            auto bwd = backward.find(inc);
            if(bwd != backward.end()) {
                bwd->second.erase(path_id);
                if(bwd->second.empty())
                    backward.erase(bwd);
            }
        }
        forward.erase(it);
    }

    auto bwd = backward.find(path_id);
    if(bwd != backward.end()) {
        for(auto inc_by : bwd->second) {
            auto fwd = forward.find(inc_by);
            if(fwd != forward.end()) {
                fwd->second.erase(path_id);
            }
        }
        backward.erase(bwd);
    }
}

llvm::SmallVector<std::uint32_t>
FuzzyGraph::get_includes(std::uint32_t path_id) const {
    auto it = forward.find(path_id);
    if(it == forward.end())
        return {};
    return {it->second.begin(), it->second.end()};
}

llvm::SmallVector<std::uint32_t>
FuzzyGraph::get_reverse_includes(std::uint32_t path_id) const {
    auto it = backward.find(path_id);
    if(it == backward.end())
        return {};
    return {it->second.begin(), it->second.end()};
}

llvm::SmallVector<std::uint32_t>
FuzzyGraph::get_affected_files(std::uint32_t path_id) const {
    llvm::DenseSet<std::uint32_t> visited;
    llvm::SmallVector<std::uint32_t> queue;
    llvm::SmallVector<std::uint32_t> result;

    queue.push_back(path_id);
    visited.insert(path_id);

    while(!queue.empty()) {
        auto current = queue.pop_back_val();
        result.push_back(current);

        auto it = backward.find(current);
        if(it == backward.end())
            continue;

        for(auto includer : it->second) {
            if(visited.insert(includer).second) {
                queue.push_back(includer);
            }
        }
    }

    return result;
}

bool FuzzyGraph::has_file(std::uint32_t path_id) const {
    return forward.count(path_id) > 0;
}

std::size_t FuzzyGraph::in_degree(std::uint32_t path_id) const {
    auto it = backward.find(path_id);
    if(it == backward.end())
        return 0;
    return it->second.size();
}

}  // namespace clice

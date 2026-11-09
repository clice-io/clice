module clice;

import :analysis.module_graph;

namespace clice::analysis {

std::vector<llvm::SmallVector<std::uint32_t>>
    strongly_connected(llvm::ArrayRef<llvm::SmallVector<std::uint32_t>> successors) {
    auto count = successors.size();
    std::vector<std::uint32_t> order(count, none), low(count, 0);
    std::vector<bool> on_stack(count, false);
    llvm::SmallVector<std::uint32_t> stack;
    std::vector<llvm::SmallVector<std::uint32_t>> components;
    std::uint32_t next = 0;
    std::function<void(std::uint32_t)> visit = [&](std::uint32_t node) {
        order[node] = low[node] = next;
        next += 1;
        stack.push_back(node);
        on_stack[node] = true;
        for(auto target: successors[node]) {
            if(order[target] == none) {
                visit(target);
                low[node] = std::min(low[node], low[target]);
            } else if(on_stack[target]) {
                low[node] = std::min(low[node], order[target]);
            }
        }
        if(low[node] != order[node]) {
            return;
        }
        auto& component = components.emplace_back();
        std::uint32_t member;
        do {
            member = stack.pop_back_val();
            on_stack[member] = false;
            component.push_back(member);
        } while(member != node);
    };
    for(std::uint32_t node = 0; node < count; node += 1) {
        if(order[node] == none) {
            visit(node);
        }
    }
    return components;
}

Reverse::Reverse(const Facts& facts) : users(facts.entities.size()), owned(facts.files.size()) {
    for(std::uint32_t file = 0; file < facts.files.size(); file += 1) {
        for(auto charged: facts.charged_files(file)) {
            for(auto* list: {&facts.uses[file], &facts.macro_uses[file]}) {
                for(auto& use: *list) {
                    auto& named = users[use.entity];
                    if(charged != facts.entities[use.entity].owner &&
                       !ranges::contains(named, charged)) {
                        named.push_back(charged);
                    }
                }
            }
        }
    }
    for(std::uint32_t entity = 0; entity < facts.entities.size(); entity += 1) {
        if(facts.entities[entity].kind != SymbolKind::Macro) {
            owned[facts.entities[entity].owner].push_back(entity);
        }
    }
}

Graph::Graph(const Facts& facts, const Partition& partition) :
    facts(facts), partition(partition), reverse(facts), dependents(facts.files.size()),
    internal(facts.files.size(), false) {
    auto count = partition.modules.size();
    // A file reaching the provider through its includes as well may
    // need the definition.
    auto reaches = [&](std::uint32_t file, std::uint32_t target) {
        llvm::DenseSet<std::uint32_t> visited{file};
        llvm::SmallVector<std::uint32_t> pending{file};
        while(!pending.empty()) {
            for(auto included: facts.files[pending.pop_back_val()].includes) {
                if(included == target) {
                    return true;
                }
                if(visited.insert(included).second) {
                    pending.push_back(included);
                }
            }
        }
        return false;
    };
    for(auto& redeclaration: facts.redeclarations) {
        if(!redeclaration.definition &&
           !reaches(redeclaration.file, facts.entities[redeclaration.entity].owner)) {
            forward_declared.insert({redeclaration.file, redeclaration.entity});
        }
    }
    for(std::uint32_t file = 0; file < facts.files.size(); file += 1) {
        for(auto entity: reverse.owned[file]) {
            for(auto user: reverse.users[entity]) {
                if(ranges::contains(dependents[file], user) ||
                   (forward_declared.contains({user, entity}) &&
                    partition.module_of[user] == partition.module_of[file])) {
                    continue;
                }
                dependents[file].push_back(user);
            }
        }
    }
    // A header owning nothing other files name, an umbrella over other
    // headers, is internal too: its includers reach what it includes
    // through those headers' partitions.
    for(std::uint32_t file = 0; file < facts.files.size(); file += 1) {
        auto& info = facts.files[file];
        internal[file] = !info.source && !info.fragment && info.specializes.empty() &&
                         ranges::all_of(dependents[file], [&](std::uint32_t user) {
                             return partition.module_of[user] == partition.module_of[file];
                         });
    }
    // A header the module's interface uses or includes cannot stay
    // internal, nor one another module includes: an interface importing an
    // implementation partition leaves its names unreachable to importers.
    auto interface = [&](std::uint32_t file) {
        return !facts.files[file].source && !facts.files[file].fragment && !internal[file];
    };
    for(bool changed = true; changed;) {
        changed = false;
        for(std::uint32_t file = 0; file < facts.files.size(); file += 1) {
            if(internal[file] &&
               (ranges::any_of(dependents[file], interface) ||
                ranges::any_of(facts.files[file].includers, [&](std::uint32_t by) {
                    return partition.module_of[by] != partition.module_of[file] || interface(by);
                }))) {
                internal[file] = false;
                changed = true;
            }
        }
    }

    imports.resize(count);
    for(std::uint32_t file = 0; file < facts.files.size(); file += 1) {
        for(auto user: facts.charged_files(file)) {
            auto from = partition.module_of[user];
            for(auto& use: facts.uses[file]) {
                auto to = partition.module_of[facts.entities[use.entity].owner];
                if(from == to) {
                    continue;
                }
                auto& edge = edges[{from, to}];
                auto& files = edge.entities[use.entity];
                if(!ranges::contains(files, user)) {
                    files.push_back(user);
                }
                if(!facts.files[user].source && !internal[user] &&
                   edge.interface.insert(use.entity).second && edge.interface.size() == 1) {
                    imports[from].push_back(to);
                }
            }
        }
    }

    components = strongly_connected(imports);
    component_of.assign(count, none);
    for(std::uint32_t c = 0; c < components.size(); c += 1) {
        for(auto module: components[c]) {
            component_of[module] = c;
        }
    }
    std::vector<std::uint32_t> component_layer(components.size(), 0);
    for(std::uint32_t c = 0; c < components.size(); c += 1) {
        for(auto module: components[c]) {
            for(auto target: imports[module]) {
                if(component_of[target] != c) {
                    component_layer[c] =
                        std::max(component_layer[c], component_layer[component_of[target]] + 1);
                }
            }
        }
    }
    layer.resize(count);
    for(std::uint32_t module = 0; module < count; module += 1) {
        layer[module] = component_layer[component_of[module]];
    }
}

std::uint32_t Graph::weight(std::uint32_t from, std::uint32_t to) const {
    auto it = edges.find({from, to});
    return it == edges.end() ? 0 : static_cast<std::uint32_t>(it->second.interface.size());
}

std::uint32_t Graph::cyclic_modules() const {
    std::uint32_t count = 0;
    for(auto& component: components) {
        if(component.size() > 1) {
            count += static_cast<std::uint32_t>(component.size());
        }
    }
    return count;
}

llvm::SmallVector<std::pair<std::uint32_t, std::uint32_t>>
    Graph::cut(llvm::ArrayRef<std::uint32_t> members) const {
    auto k = members.size();
    llvm::SmallVector<std::uint32_t> order;
    if(k <= 16) {
        auto full = (1u << k) - 1;
        std::vector<std::uint64_t> best(full + 1, std::numeric_limits<std::uint64_t>::max());
        std::vector<std::uint32_t> last(full + 1, none);
        best[0] = 0;
        for(std::uint32_t set = 0; set < full; set += 1) {
            if(best[set] == std::numeric_limits<std::uint64_t>::max()) {
                continue;
            }
            for(std::uint32_t v = 0; v < k; v += 1) {
                if(set & (1u << v)) {
                    continue;
                }
                // Placing v above every module in `set`: their edges
                // into v point upward and must go.
                std::uint64_t cost = best[set];
                for(std::uint32_t u = 0; u < k; u += 1) {
                    if(set & (1u << u)) {
                        cost += weight(members[u], members[v]);
                    }
                }
                auto next = set | (1u << v);
                if(cost < best[next]) {
                    best[next] = cost;
                    last[next] = v;
                }
            }
        }
        for(auto set = full; set != 0; set &= ~(1u << last[set])) {
            order.push_back(members[last[set]]);
        }
        ranges::reverse(order);
    } else {
        llvm::SmallVector<std::uint32_t> rest(members);
        while(!rest.empty()) {
            auto lowest = ranges::min_element(rest, {}, [&](std::uint32_t v) {
                std::uint64_t out = 0;
                for(auto u: rest) {
                    out += weight(v, u);
                }
                return out;
            });
            order.push_back(*lowest);
            rest.erase(lowest);
        }
    }

    llvm::SmallVector<std::pair<std::uint32_t, std::uint32_t>> result;
    for(std::size_t lower = 0; lower < order.size(); lower += 1) {
        for(std::size_t upper = lower + 1; upper < order.size(); upper += 1) {
            if(weight(order[lower], order[upper]) != 0) {
                result.emplace_back(order[lower], order[upper]);
            }
        }
    }
    return result;
}

}  // namespace clice::analysis

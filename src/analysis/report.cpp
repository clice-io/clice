module clice;

import :analysis.impact;
import :analysis.interface;
import :analysis.module_graph;
import :analysis.report;
import :index.symbol_query;

namespace clice::analysis {

namespace {

/// The entity names an edge carries, most-named first.
std::vector<std::string> sample(const Facts& facts, const Graph::Edge& edge, std::size_t limit) {
    std::vector<std::pair<std::uint32_t, std::size_t>> ranked;
    for(auto& [entity, files]: edge.entities) {
        ranked.emplace_back(entity, files.size());
    }
    ranges::stable_sort(ranked, ranges::greater{}, [](auto& entry) { return entry.second; });
    std::vector<std::string> names;
    for(auto& entry: ranked) {
        auto& name = facts.entities[entry.first].name;
        if(!ranges::contains(names, name)) {
            names.push_back(name);
        }
        if(names.size() == limit) {
            break;
        }
    }
    return names;
}

bool by_entities(const ModuleLink& lhs, const ModuleLink& rhs) {
    return std::tie(rhs.entities, lhs.module) < std::tie(lhs.entities, rhs.module);
}

std::string component_label(const Graph& graph, std::uint32_t component) {
    llvm::SmallVector<llvm::StringRef> names;
    for(auto module: graph.components[component]) {
        names.push_back(graph.partition.modules[module]);
    }
    ranges::sort(names);
    return llvm::join(names, " + ");
}

}  // namespace

Overview Report::overview(std::uint32_t limit) const {
    Graph graph(facts, partition);
    Overview result;
    for(auto& annotation: annotations.list) {
        result.annotations.push_back(annotation.name);
    }
    result.files = static_cast<std::uint32_t>(facts.files.size());

    for(std::uint32_t module = 0; module < partition.modules.size(); module += 1) {
        result.modules.push_back({.name = partition.modules[module], .layer = graph.layer[module]});
    }
    for(std::uint32_t file = 0; file < facts.files.size(); file += 1) {
        auto& summary = result.modules[partition.module_of[file]];
        (facts.files[file].source ? summary.sources : summary.headers) += 1;
        summary.lines += facts.files[file].lines;
    }
    ranges::sort(result.modules, [](auto& lhs, auto& rhs) {
        return std::tie(rhs.layer, lhs.name) < std::tie(lhs.layer, rhs.name);
    });

    for(auto& [key, edge]: graph.edges) {
        result.edges.push_back({
            .from = partition.modules[key.first],
            .to = partition.modules[key.second],
            .interface_entities = static_cast<std::uint32_t>(edge.interface.size()),
            .implementation_entities =
                static_cast<std::uint32_t>(edge.entities.size() - edge.interface.size()),
            .sample = sample(facts, edge, 5),
        });
    }
    ranges::sort(result.edges, [](auto& lhs, auto& rhs) {
        return std::tie(rhs.interface_entities, rhs.implementation_entities, lhs.from, lhs.to) <
               std::tie(lhs.interface_entities, lhs.implementation_entities, rhs.from, rhs.to);
    });

    result.cyclic_modules = graph.cyclic_modules();
    for(auto& component: graph.components) {
        if(component.size() < 2) {
            continue;
        }
        auto& cycle = result.cycles.emplace_back();
        for(auto module: component) {
            cycle.modules.push_back(partition.modules[module]);
        }
        ranges::sort(cycle.modules);
        for(auto [from, to]: graph.cut(component)) {
            auto& edge = graph.edges.at({from, to});
            auto& cut = cycle.cut.emplace_back();
            cut.from = partition.modules[from];
            cut.to = partition.modules[to];
            cut.entities = static_cast<std::uint32_t>(edge.interface.size());
            Graph::Edge interface;
            llvm::DenseSet<std::uint32_t> files;
            for(auto& [entity, users]: edge.entities) {
                if(!edge.interface.contains(entity)) {
                    continue;
                }
                interface.entities[entity] = users;
                for(auto user: users) {
                    if(!facts.files[user].source && !graph.internal[user] &&
                       files.insert(user).second) {
                        cut.files.push_back(facts.files[user].path);
                    }
                }
            }
            cut.sample = sample(facts, interface, 8);
            ranges::sort(cut.files);
        }
    }

    // Header-level cycles inside each module. A use backed by the user's
    // own forward declaration needs no import inside one module.
    std::vector<llvm::SmallVector<std::uint32_t>> header_uses(facts.files.size());
    for(std::uint32_t file = 0; file < facts.files.size(); file += 1) {
        for(auto header: facts.charged_files(file)) {
            if(facts.files[header].source) {
                continue;
            }
            for(auto& use: facts.uses[file]) {
                auto owner = facts.entities[use.entity].owner;
                if(owner != header && partition.module_of[owner] == partition.module_of[header] &&
                   !facts.files[owner].source &&
                   !graph.forward_declared.contains({file, use.entity}) &&
                   !ranges::contains(header_uses[header], owner)) {
                    header_uses[header].push_back(owner);
                }
            }
        }
    }
    for(auto& component: strongly_connected(header_uses)) {
        if(component.size() < 2) {
            continue;
        }
        auto& cycle = result.partition_cycles.emplace_back();
        cycle.module = partition.modules[partition.module_of[component.front()]];
        for(auto file: component) {
            cycle.headers.push_back(facts.files[file].path);
        }
        ranges::sort(cycle.headers);
    }
    ranges::sort(result.partition_cycles, {}, &PartitionCycle::module);
    result.internal_headers = static_cast<std::uint32_t>(ranges::count(graph.internal, true));

    if(!graph.components.empty()) {
        // Down from the highest component through the import that sets
        // each one's layer.
        auto top = ranges::max_element(
            views::iota(std::uint32_t(0), static_cast<std::uint32_t>(graph.components.size())),
            {},
            [&](std::uint32_t c) { return graph.layer[graph.components[c].front()]; });
        for(auto c = *top;;) {
            result.deepest_chain.push_back(component_label(graph, c));
            auto current = graph.layer[graph.components[c].front()];
            if(current == 0) {
                break;
            }
            auto next = none;
            for(auto module: graph.components[c]) {
                for(auto target: graph.imports[module]) {
                    if(graph.component_of[target] != c && graph.layer[target] + 1 == current) {
                        next = graph.component_of[target];
                    }
                }
            }
            c = next;
        }
    }

    Rebuild rebuild(graph, annotations);
    auto* churn = annotations.find(churn_annotation);
    for(std::uint32_t file = 0; file < facts.files.size(); file += 1) {
        auto edits = churn ? churn->values.lookup(facts.files[file].path) : 1.0;
        result.totals.baseline += edits * rebuild.baseline[file];
        result.totals.partitioned += edits * rebuild.partitioned[file];
    }
    result.hotspots = impact();
    if(result.hotspots.size() > limit) {
        result.hotspots.resize(limit);
    }

    // A header private to one other module: with the sources implementing
    // it and the other headers those implement, it exchanges nothing with
    // the rest of its own module, and all its users sit in that other
    // module. A header specializing out-of-scope templates has users no
    // relation shows and stays put.
    auto& reverse = graph.reverse;
    llvm::DenseMap<std::uint32_t, llvm::SmallVector<std::uint32_t, 2>> implemented_by, implements;
    for(auto& redeclaration: facts.redeclarations) {
        auto header = facts.entities[redeclaration.entity].owner;
        if(!redeclaration.definition || !facts.files[redeclaration.file].source ||
           facts.files[header].source) {
            continue;
        }
        if(auto& sources = implemented_by[header]; !ranges::contains(sources, redeclaration.file)) {
            sources.push_back(redeclaration.file);
            implements[redeclaration.file].push_back(header);
        }
    }
    // A macro ties its header to its users like any entity.
    std::vector<llvm::SmallVector<std::uint32_t>> macros(facts.files.size());
    for(std::uint32_t entity = 0; entity < facts.entities.size(); entity += 1) {
        if(facts.entities[entity].kind == SymbolKind::Macro) {
            macros[facts.entities[entity].owner].push_back(entity);
        }
    }
    std::vector<bool> visited(facts.files.size(), false);
    for(std::uint32_t file = 0; file < facts.files.size(); file += 1) {
        if(facts.files[file].source || facts.files[file].fragment || visited[file]) {
            continue;
        }
        llvm::SmallVector<std::uint32_t, 4> unit{file};
        visited[file] = true;
        for(std::size_t next = 0; next < unit.size(); next += 1) {
            auto current = unit[next];
            auto& linked = facts.files[current].source ? implements : implemented_by;
            for(auto other: linked.lookup(current)) {
                if(!visited[other]) {
                    visited[other] = true;
                    unit.push_back(other);
                }
            }
        }
        auto home = partition.module_of[file];
        if(ranges::any_of(unit, [&](std::uint32_t member) {
               auto& info = facts.files[member];
               return info.fragment || !info.specializes.empty() ||
                      partition.module_of[member] != home;
           })) {
            continue;
        }
        llvm::SmallDenseMap<std::uint32_t, llvm::DenseSet<std::uint32_t>, 8> exchanged;
        llvm::SmallDenseSet<std::uint32_t, 4> user_modules;
        for(auto member: unit) {
            for(auto& use: llvm::concat<const Use>(facts.uses[member], facts.macro_uses[member])) {
                auto owner = facts.entities[use.entity].owner;
                if(!ranges::contains(unit, owner)) {
                    exchanged[partition.module_of[owner]].insert(use.entity);
                }
            }
            for(auto entity:
                llvm::concat<const std::uint32_t>(reverse.owned[member], macros[member])) {
                for(auto user: reverse.users[entity]) {
                    if(!ranges::contains(unit, user)) {
                        exchanged[partition.module_of[user]].insert(entity);
                        user_modules.insert(partition.module_of[user]);
                    }
                }
            }
        }
        // Moving the whole module is a merge.
        if(exchanged.contains(home) || user_modules.size() != 1 ||
           ranges::count(partition.module_of, home) == unit.size()) {
            continue;
        }
        ranges::sort(unit, {}, [&](std::uint32_t member) {
            return std::tie(facts.files[member].source, facts.files[member].path);
        });
        auto target = *user_modules.begin();
        auto& move = result.moves.emplace_back();
        move.path = facts.files[unit.front()].path;
        move.from = partition.modules[home];
        move.to = partition.modules[target];
        for(auto member: views::drop(unit, 1)) {
            move.with.push_back(facts.files[member].path);
        }
        move.entities = static_cast<std::uint32_t>(exchanged[target].size());
    }
    ranges::sort(result.moves, [](auto& lhs, auto& rhs) {
        return std::tie(rhs.entities, lhs.path) < std::tie(lhs.entities, rhs.path);
    });
    if(result.moves.size() > limit) {
        result.moves.resize(limit);
    }
    for(auto& move: result.moves) {
        auto moved = partition;
        auto to = partition.module_named(move.to);
        moved.module_of[facts.file_ids.lookup(move.path)] = to;
        for(auto& path: move.with) {
            moved.module_of[facts.file_ids.lookup(path)] = to;
        }
        move.cyclic_after = Graph(facts, moved).cyclic_modules();
    }

    // A header whose externally used entities fall into groups no
    // consuming module shares: each consumer pulls its entities together,
    // and a member pulls its class.
    for(std::uint32_t file = 0; file < facts.files.size(); file += 1) {
        if(facts.files[file].source || facts.files[file].fragment) {
            continue;
        }
        auto home = partition.module_of[file];
        llvm::EquivalenceClasses<std::uint32_t> groups;
        llvm::DenseMap<std::uint32_t, std::uint32_t> first_of_module;
        llvm::DenseMap<std::uint32_t, llvm::SmallDenseSet<std::uint32_t, 4>> consumers;
        for(auto entity: reverse.owned[file]) {
            auto top = facts.entities[entity].top;
            for(auto user: reverse.users[entity]) {
                auto module = partition.module_of[user];
                if(module == home) {
                    continue;
                }
                groups.insert(top);
                consumers[top].insert(module);
                auto [it, inserted] = first_of_module.try_emplace(module, top);
                if(!inserted) {
                    groups.unionSets(it->second, top);
                }
            }
        }
        llvm::SmallVector<SplitPart> parts;
        for(auto* group: groups) {
            if(!group->isLeader()) {
                continue;
            }
            auto& part = parts.emplace_back();
            std::set<std::string> modules;
            auto range = groups.members(*group);
            llvm::SmallVector<std::uint32_t> members(range.begin(), range.end());
            ranges::sort(members, {}, [&](std::uint32_t member) {
                return std::tie(facts.entities[member].line, facts.entities[member].name);
            });
            part.count = static_cast<std::uint32_t>(members.size());
            for(auto member: members) {
                if(part.entities.size() < 10) {
                    auto& entity = facts.entities[member];
                    part.entities.push_back(std::format("{}:{}", entity.name, entity.line));
                }
                for(auto module: consumers[member]) {
                    modules.insert(partition.modules[module]);
                }
            }
            part.consumers.assign(modules.begin(), modules.end());
        }
        if(parts.size() > 1) {
            ranges::sort(parts, [](auto& lhs, auto& rhs) {
                return std::tuple(rhs.consumers.size(), lhs.consumers, lhs.entities) <
                       std::tuple(lhs.consumers.size(), rhs.consumers, rhs.entities);
            });
            result.splits.push_back({
                .path = facts.files[file].path,
                .module = partition.modules[home],
                .parts = {parts.begin(), parts.end()},
            });
        }
    }
    ranges::sort(result.splits, [](auto& lhs, auto& rhs) {
        return std::tuple(rhs.parts.size(), lhs.path) < std::tuple(lhs.parts.size(), rhs.path);
    });
    if(result.splits.size() > limit) {
        result.splits.resize(limit);
    }

    auto blocked = obstacles();
    result.obstacles = {
        .variant_headers = static_cast<std::uint32_t>(
            ranges::count_if(blocked.variant_headers, &VariantHeader::declarations_differ)),
        .context_macros = static_cast<std::uint32_t>(blocked.context_macros.size()),
        .borrowed_macros = static_cast<std::uint32_t>(blocked.borrowed_macros.size()),
        .internal_uses = static_cast<std::uint32_t>(blocked.internal_uses.size()),
        .cross_module_redeclarations =
            static_cast<std::uint32_t>(blocked.cross_module_redeclarations.size()),
        .foreign_declarations = static_cast<std::uint32_t>(blocked.foreign_declarations.size()),
        .split_definitions = static_cast<std::uint32_t>(blocked.split_definitions.size()),
        .duplicate_definitions = static_cast<std::uint32_t>(blocked.duplicate_definitions.size()),
        .configuring_macros = static_cast<std::uint32_t>(blocked.configuring_macros.size()),
        .implicit_providers = static_cast<std::uint32_t>(blocked.implicit_providers.size()),
        .specializations = static_cast<std::uint32_t>(blocked.specializations.size()),
    };
    return result;
}

std::expected<EdgeDetail, std::string> Report::edge(llvm::StringRef from,
                                                    llvm::StringRef to) const {
    auto source = partition.module_named(from);
    auto target = partition.module_named(to);
    if(source == partition.modules.size() || target == partition.modules.size()) {
        return std::unexpected(
            std::format("no module {}",
                        std::string_view(source == partition.modules.size() ? from : to)));
    }
    Graph graph(facts, partition);
    EdgeDetail result{.from = from.str(), .to = to.str()};
    auto it = graph.edges.find({source, target});
    if(it == graph.edges.end()) {
        return result;
    }
    // The line of the first use a file's uses (or a fragment it pastes in)
    // hold of the entity.
    auto pasted = facts.pasted_fragments();
    auto line_in = [&](std::uint32_t user, std::uint32_t entity) {
        for(auto file: llvm::concat<const std::uint32_t>(llvm::ArrayRef(user), pasted[user])) {
            auto& uses = facts.uses[file];
            auto use = ranges::lower_bound(uses, entity, {}, &Use::entity);
            if(use != uses.end() && use->entity == entity) {
                return use->line == 0 ? facts.files[file].path
                                      : std::format("{}:{}", facts.files[file].path, use->line);
            }
        }
        return facts.files[user].path;
    };
    for(auto& [entity, files]: it->second.entities) {
        auto& entry = result.entities.emplace_back();
        auto& info = facts.entities[entity];
        entry.id = index::symbol_locator(info.hash,
                                         info.linkage == InternalLinkage::None
                                             ? llvm::StringRef()
                                             : llvm::StringRef(facts.files[info.declared_in].path));
        entry.entity = info.name;
        entry.owner = facts.files[info.owner].path;
        entry.interface = it->second.interface.contains(entity);
        for(auto file: files) {
            entry.users.push_back(line_in(file, entity));
        }
        ranges::sort(entry.users);
    }
    ranges::sort(result.entities, [](auto& lhs, auto& rhs) {
        return std::tuple(lhs.interface, lhs.users.size(), rhs.entity) >
               std::tuple(rhs.interface, rhs.users.size(), lhs.entity);
    });
    return result;
}

std::expected<ModuleDetail, std::string> Report::module(llvm::StringRef name) const {
    auto module = partition.module_named(name);
    if(module == partition.modules.size()) {
        return std::unexpected(std::format("no module {}", std::string_view(name)));
    }
    Graph graph(facts, partition);
    auto& reverse = graph.reverse;
    ModuleDetail result{.name = name.str()};
    auto links = [&](auto& by_module) {
        std::vector<ModuleLink> result;
        for(auto& [other, entities]: by_module) {
            result.push_back({.module = partition.modules[other],
                              .entities = static_cast<std::uint32_t>(entities.size())});
        }
        ranges::sort(result, by_entities);
        return result;
    };
    // A header's consumers are the other modules using it, directly or
    // through the module's headers using it.
    llvm::DenseMap<std::uint32_t, std::set<std::uint32_t>> consumers;
    auto pasted = facts.pasted_fragments();
    for(std::uint32_t file = 0; file < facts.files.size(); file += 1) {
        if(partition.module_of[file] != module) {
            continue;
        }
        llvm::SmallDenseMap<std::uint32_t, llvm::DenseSet<std::uint32_t>, 8> uses, used_by;
        for(auto named: llvm::concat<const std::uint32_t>(llvm::ArrayRef(file), pasted[file])) {
            for(auto& use: facts.uses[named]) {
                auto owner = partition.module_of[facts.entities[use.entity].owner];
                if(owner != module) {
                    uses[owner].insert(use.entity);
                }
            }
        }
        for(auto entity: reverse.owned[file]) {
            for(auto user: reverse.users[entity]) {
                if(partition.module_of[user] != module) {
                    used_by[partition.module_of[user]].insert(entity);
                }
            }
        }
        result.files.push_back({
            .path = facts.files[file].path,
            .source = facts.files[file].source,
            .uses = links(uses),
            .used_by = links(used_by),
        });
        if(facts.files[file].source || facts.files[file].fragment) {
            continue;
        }
        if(graph.internal[file]) {
            result.internal_headers.push_back(facts.files[file].path);
            continue;
        }
        auto& own = consumers[file];
        for(auto& [other, entities]: used_by) {
            own.insert(other);
        }
    }
    for(bool changed = true; changed;) {
        changed = false;
        for(auto& [file, own]: consumers) {
            for(auto user: graph.dependents[file]) {
                auto it = consumers.find(user);
                if(it == consumers.end()) {
                    continue;
                }
                for(auto other: it->second) {
                    changed |= own.insert(other).second;
                }
            }
        }
    }
    std::map<std::set<std::uint32_t>, std::vector<std::uint32_t>> grouped;
    for(auto& [file, own]: consumers) {
        grouped[own].push_back(file);
    }
    for(auto& [modules, files]: grouped) {
        auto& group = result.groups.emplace_back();
        std::set<std::string> depends_on;
        for(auto other: modules) {
            group.consumers.push_back(partition.modules[other]);
        }
        for(auto file: files) {
            group.headers.push_back(facts.files[file].path);
            for(auto named: llvm::concat<const std::uint32_t>(llvm::ArrayRef(file), pasted[file])) {
                for(auto& use: facts.uses[named]) {
                    auto owner = partition.module_of[facts.entities[use.entity].owner];
                    if(owner != module) {
                        depends_on.insert(partition.modules[owner]);
                    }
                }
            }
        }
        ranges::sort(group.consumers);
        ranges::sort(group.headers);
        group.depends_on.assign(depends_on.begin(), depends_on.end());
    }
    ranges::sort(result.groups, [](auto& lhs, auto& rhs) {
        return std::tuple(rhs.consumers.size(), lhs.headers) <
               std::tuple(lhs.consumers.size(), rhs.headers);
    });
    ranges::sort(result.files, {}, &FileLinks::path);
    ranges::sort(result.internal_headers);
    return result;
}

std::expected<FileDetail, std::string> Report::file(llvm::StringRef path) const {
    auto it = facts.file_ids.find(path);
    if(it == facts.file_ids.end()) {
        return std::unexpected(std::format("no scoped file {}", std::string_view(path)));
    }
    auto file = it->second;
    auto home = partition.module_of[file];
    FileDetail result{
        .path = path.str(),
        .module = partition.modules[home],
        .source = facts.files[file].source,
    };
    for(auto& annotation: annotations.list) {
        if(auto value = annotation.values.find(path); value != annotation.values.end()) {
            result.annotations.push_back(
                {.name = annotation.name, .unit = annotation.unit, .value = value->second});
        }
    }

    std::map<std::uint32_t, std::set<std::string>> uses, used_by;
    std::map<std::uint32_t, llvm::DenseSet<std::uint32_t>> exchanged;
    auto pasted = facts.pasted_fragments();
    for(auto named: llvm::concat<const std::uint32_t>(llvm::ArrayRef(file), pasted[file])) {
        for(auto& use: facts.uses[named]) {
            auto module = partition.module_of[facts.entities[use.entity].owner];
            uses[module].insert(facts.entities[use.entity].name);
            exchanged[module].insert(use.entity);
        }
    }
    Reverse reverse(facts);
    for(auto entity: reverse.owned[file]) {
        for(auto user: reverse.users[entity]) {
            used_by[partition.module_of[user]].insert(facts.entities[entity].name);
            exchanged[partition.module_of[user]].insert(entity);
        }
    }
    auto named = [&](auto& by_module) {
        std::vector<NamedUses> result;
        for(auto& [module, names]: by_module) {
            result.push_back({
                .module = partition.modules[module],
                .entities = {names.begin(), names.end()}
            });
        }
        return result;
    };
    result.uses = named(uses);
    result.used_by = named(used_by);
    for(auto& [module, entities]: exchanged) {
        result.affinity.push_back({.module = partition.modules[module],
                                   .entities = static_cast<std::uint32_t>(entities.size())});
    }
    ranges::sort(result.affinity, by_entities);
    return result;
}

Obstacles Report::obstacles() const {
    Graph graph(facts, partition);
    auto& module_of = partition.module_of;
    Obstacles result;
    for(auto& file: facts.files) {
        if(file.source || file.fragment || file.variants < 2) {
            continue;
        }
        auto& header = result.variant_headers.emplace_back();
        header.path = file.path;
        header.variants = file.variants;
        header.units = static_cast<std::uint32_t>(file.units.size());
        header.unstable.assign(file.unstable.begin(),
                               file.unstable.begin() +
                                   std::min<std::size_t>(file.unstable.size(), 10));
        header.declarations_differ = file.declarations_differ;
    }
    ranges::sort(result.variant_headers, [](auto& lhs, auto& rhs) {
        return std::tie(rhs.declarations_differ, rhs.variants, lhs.path) <
               std::tie(lhs.declarations_differ, lhs.variants, rhs.path);
    });

    for(auto& macro: facts.context_macros) {
        (macro.in_condition ? result.context_macros : result.borrowed_macros)
            .push_back({
                .macro = macro.name,
                .definition = macro.definition,
                .file = facts.files[macro.file].path,
                .line = macro.line,
            });
    }

    auto& reverse = graph.reverse;
    for(std::uint32_t entity = 0; entity < facts.entities.size(); entity += 1) {
        auto& info = facts.entities[entity];
        auto tu_local = info.linkage == InternalLinkage::Static ||
                        info.linkage == InternalLinkage::AnonymousNamespace;
        if(!tu_local || facts.files[info.owner].source ||
           (reverse.users[entity].empty() && info.self_uses == 0)) {
            continue;
        }
        auto& use = result.internal_uses.emplace_back();
        use.entity = info.name;
        use.owner = facts.files[info.owner].path;
        use.reason = info.linkage == InternalLinkage::Static ? "static" : "anonymous namespace";
        use.exposed_in_owner = info.self_uses != 0;
        for(auto user: reverse.users[entity]) {
            use.users.push_back(facts.files[user].path);
        }
        ranges::sort(use.users);
    }

    for(auto& redeclaration: facts.redeclarations) {
        auto& info = facts.entities[redeclaration.entity];
        auto from = module_of[redeclaration.file];
        auto to = module_of[info.owner];
        if(from == to) {
            continue;
        }
        if(redeclaration.definition) {
            if(facts.files[redeclaration.file].source && !facts.files[info.owner].source) {
                result.split_definitions.push_back({
                    .name = info.name,
                    .related = facts.files[info.owner].path,
                    .file = facts.files[redeclaration.file].path,
                    .line = redeclaration.line,
                });
            }
            continue;
        }
        auto named = ranges::any_of(facts.uses[redeclaration.file], [&](const Use& use) {
            return use.entity == redeclaration.entity;
        });
        result.cross_module_redeclarations.push_back({
            .entity = info.name,
            .owner = facts.files[info.owner].path,
            .file = facts.files[redeclaration.file].path,
            .line = redeclaration.line,
            .friend_declaration = redeclaration.friend_declaration,
            .unused = !named,
            .on_cycle = named && graph.component_of[from] == graph.component_of[to],
        });
    }

    for(auto& declaration: facts.foreign_declarations) {
        result.foreign_declarations.push_back({
            .name = declaration.name,
            .owner = declaration.owner,
            .file = facts.files[declaration.file].path,
            .line = declaration.line,
            .friend_declaration = declaration.friend_declaration,
        });
    }

    for(auto& duplicate: facts.duplicate_definitions) {
        auto& entry = result.duplicate_definitions.emplace_back();
        entry.entity = facts.entities[duplicate.entity].name;
        for(auto file: duplicate.files) {
            entry.files.push_back(facts.files[file].path);
        }
        ranges::sort(entry.files);
    }
    ranges::sort(result.duplicate_definitions, {}, &DuplicateEntry::entity);

    for(auto& specialization: facts.specializations) {
        auto& primary = facts.entities[specialization.primary];
        if(module_of[specialization.file] != module_of[primary.owner]) {
            result.specializations.push_back({
                .name = primary.name,
                .related = facts.files[primary.owner].path,
                .file = facts.files[specialization.file].path,
                .line = specialization.line,
            });
        }
    }

    for(auto& macro: facts.configuring_macros) {
        auto& info = facts.entities[macro.entity];
        result.configuring_macros.push_back({
            .macro = info.name,
            .definition = facts.files[info.owner].path,
            .readers = macro.readers,
        });
    }
    ranges::sort(result.configuring_macros, {}, &ConfiguringEntry::macro);

    for(auto& file: facts.files) {
        if(!file.specializes.empty()) {
            result.implicit_providers.push_back(
                {.path = file.path, .specializes = file.specializes});
        }
    }
    ranges::sort(result.implicit_providers, {}, &ProviderEntry::path);

    auto by_file = [](auto& entries) {
        ranges::sort(entries, [](auto& lhs, auto& rhs) {
            return std::tie(lhs.file, lhs.line) < std::tie(rhs.file, rhs.line);
        });
    };
    by_file(result.context_macros);
    by_file(result.borrowed_macros);
    by_file(result.cross_module_redeclarations);
    by_file(result.foreign_declarations);
    by_file(result.split_definitions);
    by_file(result.specializations);
    ranges::sort(result.internal_uses, [](auto& lhs, auto& rhs) {
        return std::tie(lhs.owner, lhs.entity) < std::tie(rhs.owner, rhs.entity);
    });
    return result;
}

std::vector<MacroUse> Report::macros() const {
    std::vector<MacroUse> result;
    Reverse reverse(facts);
    for(std::uint32_t entity = 0; entity < facts.entities.size(); entity += 1) {
        auto& info = facts.entities[entity];
        if(info.kind != SymbolKind::Macro || reverse.users[entity].empty()) {
            continue;
        }
        std::map<std::uint32_t, std::uint32_t> files;
        for(auto user: reverse.users[entity]) {
            files[partition.module_of[user]] += 1;
        }
        auto& use = result.emplace_back();
        use.macro = info.name;
        use.definition = facts.files[info.owner].path;
        for(auto [module, count]: files) {
            use.users.push_back({.module = partition.modules[module], .entities = count});
        }
    }
    ranges::sort(result, [](auto& lhs, auto& rhs) {
        return std::tuple(lhs.users.size(), rhs.macro) > std::tuple(rhs.users.size(), lhs.macro);
    });
    return result;
}

std::expected<std::vector<Interface>, std::string> Report::interface(llvm::StringRef name) const {
    auto all = interfaces(facts, partition);
    if(name.empty()) {
        return all;
    }
    auto found = ranges::find(all, name, &Interface::module);
    if(found == all.end()) {
        return std::unexpected(std::format("no module {}", name.str()));
    }
    return std::vector{std::move(*found)};
}

}  // namespace clice::analysis

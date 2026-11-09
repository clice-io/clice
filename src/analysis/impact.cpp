module clice;

import :analysis.impact;
import :analysis.report;

namespace clice::analysis {

Rebuild::Rebuild(const Graph& graph, const Annotations& annotations) {
    auto& facts = graph.facts;
    auto& module_of = graph.partition.module_of;
    auto count = graph.partition.modules.size();
    auto cost = [&](std::uint32_t file) {
        return annotations.value(compile_time_annotation, facts.files[file].path, 1.0);
    };

    std::vector<llvm::SmallVector<std::uint32_t>> importers(count);
    for(std::uint32_t module = 0; module < count; module += 1) {
        for(auto target: graph.imports[module]) {
            importers[target].push_back(module);
        }
    }
    // An edit to a source or an internal partition rebuilds the units
    // entering it: the rewrite imports a partition wherever its header was
    // included. Those units name the modules it names.
    auto imported = [&](std::uint32_t user) {
        return facts.files[user].source || graph.internal[user];
    };
    std::vector<llvm::SmallDenseSet<std::uint32_t, 8>> named(facts.files.size());
    for(std::uint32_t file = 0; file < facts.files.size(); file += 1) {
        for(auto user: facts.charged_files(file)) {
            if(!imported(user)) {
                continue;
            }
            for(auto unit: facts.files[user].units) {
                for(auto& use: facts.uses[file]) {
                    named[unit].insert(module_of[facts.entities[use.entity].owner]);
                }
            }
        }
    }

    std::vector<llvm::SmallVector<std::uint32_t>> rebuilt_by_module(count);
    for(std::uint32_t module = 0; module < count; module += 1) {
        std::vector<bool> affected(count, false);
        llvm::SmallVector<std::uint32_t> pending{module};
        while(!pending.empty()) {
            auto current = pending.pop_back_val();
            if(affected[current]) {
                continue;
            }
            affected[current] = true;
            pending.append(importers[current].begin(), importers[current].end());
        }
        for(std::uint32_t file = 0; file < facts.files.size(); file += 1) {
            if(facts.files[file].source &&
               (affected[module_of[file]] ||
                ranges::any_of(named[file], [&](std::uint32_t m) { return affected[m]; }))) {
                rebuilt_by_module[module].push_back(file);
            }
        }
    }

    for(std::uint32_t file = 0; file < facts.files.size(); file += 1) {
        double today = 0;
        for(auto unit: facts.files[file].units) {
            today += cost(unit);
        }
        baseline.push_back(today);
        llvm::DenseSet<std::uint32_t> rebuilt;
        for(auto user: facts.charged_files(file)) {
            rebuilt.insert_range(imported(user)
                                     ? llvm::ArrayRef(facts.files[user].units)
                                     : llvm::ArrayRef(rebuilt_by_module[module_of[user]]));
        }
        // Summed in file order, so the floating-point total is stable.
        llvm::SmallVector<std::uint32_t> sources(rebuilt.begin(), rebuilt.end());
        ranges::sort(sources);
        double after = 0;
        for(auto source: sources) {
            after += cost(source);
        }
        partitioned.push_back(after);
    }
}

std::vector<Impact> Report::impact() const {
    Graph graph(facts, partition);
    Rebuild rebuild(graph, annotations);
    auto* churn = annotations.find(churn_annotation);
    std::vector<Impact> result;
    for(std::uint32_t file = 0; file < facts.files.size(); file += 1) {
        if(facts.files[file].source) {
            continue;
        }
        auto& impact = result.emplace_back();
        impact.path = facts.files[file].path;
        impact.baseline = rebuild.baseline[file];
        impact.partitioned = rebuild.partitioned[file];
        impact.internal = graph.internal[file];
        if(churn) {
            impact.churn = churn->values.lookup(impact.path);
        }
    }
    ranges::sort(result, [](auto& lhs, auto& rhs) {
        auto weight = [](const Impact& impact) {
            return impact.churn.value_or(1.0) * impact.partitioned;
        };
        return std::tuple(weight(lhs), rhs.path) > std::tuple(weight(rhs), lhs.path);
    });
    return result;
}

}  // namespace clice::analysis

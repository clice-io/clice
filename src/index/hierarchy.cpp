module;

#include "modules/prelude.h"

module clice;

import :index.hierarchy;
import :index.query;

namespace clice::index {

namespace {

/// A function some source defines: its definition's extent and its name.
struct DefinedFunction {
    SymbolHash hash;
    LocalSourceRange extent;
    LocalSourceRange name;
};

/// The innermost of the functions whose definition holds `range` outside
/// their name.
std::optional<SymbolHash> innermost_function(llvm::ArrayRef<DefinedFunction> functions,
                                             LocalSourceRange range) {
    const DefinedFunction* innermost = nullptr;
    for(const auto& function: functions) {
        if(function.extent.contains(range) && !function.name.contains(range) &&
           (!innermost || innermost->extent.contains(function.extent))) {
            innermost = &function;
        }
    }
    if(!innermost) {
        return std::nullopt;
    }
    return innermost->hash;
}

void collect_by_target(const IndexQuery& index,
                       SymbolHash hash,
                       Fid anchor,
                       RelationKind kind,
                       llvm::DenseMap<SymbolHash, std::vector<Site>>& by_target) {
    // The focus-file preamble entry cannot contribute: the preamble region
    // holds only preprocessor directives, never call or type relations.
    index.for_each_relation(hash,
                            anchor,
                            kind,
                            IndexQuery::Order::DiskFirst,
                            {.preamble = false},
                            [&](const IndexSource& source, const Relation& relation) {
                                if(auto site = source.site(relation.range)) {
                                    by_target[relation.target].push_back(*site);
                                }
                                return true;
                            });
}

std::vector<CallEdge> located_edges(const IndexQuery& index,
                                    llvm::DenseMap<SymbolHash, std::vector<Site>>& by_target) {
    std::vector<CallEdge> result;
    result.reserve(by_target.size());
    for(auto& [target, sites]: by_target) {
        auto located = index.resolve(target, sites.front().file);
        if(!located) {
            continue;
        }
        // A relation present in both a shard and an overlay arrives twice;
        // hierarchy items must not repeat call sites.
        dedup_sites(sites);
        result.push_back({.symbol = std::move(*located), .sites = std::move(sites)});
    }
    return result;
}

std::vector<DefinedFunction> defined_functions(const IndexQuery& index, const IndexSource& source) {
    std::vector<DefinedFunction> functions;
    source.shard->for_each_relation([&](SymbolHash symbol, const Relation& defined) {
        if(defined.kind != RelationKind::Definition || !defined.extent().contains(defined.range)) {
            return true;
        }
        if(auto info = index.symbol_info(symbol, source.file); info && is_function(info->kind)) {
            functions.push_back({
                .hash = symbol,
                .extent = defined.extent(),
                .name = defined.range,
            });
        }
        return true;
    });
    return functions;
}

std::vector<CallEdge> referencing_functions(const IndexQuery& index, SymbolHash hash, Fid anchor) {
    // Each file's function definitions, by their extents.
    llvm::DenseMap<const Shard*, std::vector<DefinedFunction>> functions;
    llvm::DenseMap<SymbolHash, std::vector<Site>> by_function;
    auto visit = [&](const IndexSource& source, const Relation& relation) {
        // An overlay's header entries come without a shard to look in.
        if(!source.shard) {
            return true;
        }
        auto [it, inserted] = functions.try_emplace(source.shard);
        if(inserted) {
            it->second = defined_functions(index, source);
        }
        if(auto function = innermost_function(it->second, relation.range)) {
            if(auto site = source.site(relation.range)) {
                by_function[*function].push_back(*site);
            }
        }
        return true;
    };
    for(auto kind: {RelationKind::Reference, RelationKind::WeakReference}) {
        index.for_each_relation(hash,
                                anchor,
                                kind,
                                IndexQuery::Order::DiskFirst,
                                {.preamble = false},
                                visit);
    }
    return located_edges(index, by_function);
}

}  // namespace

std::vector<std::string> enclosing_functions(const IndexQuery& index, llvm::ArrayRef<Site> sites) {
    llvm::DenseMap<Fid, std::vector<DefinedFunction>> functions;
    std::vector<std::string> names;
    names.reserve(sites.size());
    for(const auto& site: sites) {
        auto [it, inserted] = functions.try_emplace(site.file);
        if(inserted) {
            if(auto source = index.serving(site.file)) {
                it->second = defined_functions(index, *source);
            }
        }
        auto function = innermost_function(it->second, site.range);
        names.push_back(function ? index.qualified_name(*function, site.file) : std::string());
    }
    return names;
}

CallGraph
    call_graph(const IndexQuery& index, SymbolHash root, Fid anchor, CallGraphOptions options) {
    CallGraph graph;
    if(options.callers) {
        auto info = index.symbol_info(root, anchor);
        bool value = info && llvm::is_contained(
                                 {SymbolKind::Variable, SymbolKind::Field, SymbolKind::EnumMember},
                                 info->kind);
        if(value) {
            // A function's local has no caller but that function.
            if(info->kind != SymbolKind::Variable || info->scope != SymbolScope::FunctionLocal) {
                graph.callers = referencing_functions(index, root, anchor);
            }
        } else {
            llvm::DenseMap<SymbolHash, std::vector<Site>> by_caller;
            collect_by_target(index, root, anchor, RelationKind::Caller, by_caller);
            // A call through a method the root overrides may dispatch to it,
            // save one naming that method by qualification.
            if(info && is_function(info->kind)) {
                for(auto base: index.transitive_targets(root, anchor, RelationKind::Interface)) {
                    llvm::DenseMap<SymbolHash, std::vector<Site>> through_base;
                    collect_by_target(index,
                                      base.symbol,
                                      base.anchor,
                                      RelationKind::Caller,
                                      through_base);
                    auto direct = index.sites(base.symbol, base.anchor, RelationKind::Direct);
                    for(auto& [caller, calls]: through_base) {
                        for(auto& call: calls) {
                            // A call a macro spells sits at the whole
                            // invocation, the name it qualifies inside.
                            if(llvm::none_of(direct, [&](const Site& site) {
                                   return covers(call, site);
                               })) {
                                by_caller[caller].push_back(std::move(call));
                            }
                        }
                    }
                }
            }
            graph.callers = located_edges(index, by_caller);
        }
    }
    if(options.callees) {
        llvm::DenseMap<SymbolHash, std::vector<Site>> by_callee;
        collect_by_target(index, root, anchor, RelationKind::Callee, by_callee);
        graph.callees = located_edges(index, by_callee);
    }
    return graph;
}

TypeHierarchy type_hierarchy(const IndexQuery& index,
                             SymbolHash root,
                             Fid anchor,
                             TypeHierarchyOptions options) {
    TypeHierarchy hierarchy;
    if(options.supertypes) {
        hierarchy.supertypes = index.located_targets(root, anchor, RelationKind::Base);
    }
    if(options.subtypes) {
        hierarchy.subtypes = index.located_targets(root, anchor, RelationKind::Derived);
    }
    return hierarchy;
}

}  // namespace clice::index

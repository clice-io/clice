module;

#include "support/prelude.h"

module clice;

import :feature.document_symbols;
import :feature.hierarchy;
import :feature.protocol;
import :index.peer_queries;
import :index.query;
import :semantic.symbol;

namespace clice::feature {

namespace {

/// A function some source defines: its definition's extent and its name.
struct DefinedFunction {
    index::SymbolHash hash;
    LocalSourceRange extent;
    LocalSourceRange name;
};

/// The innermost of the functions whose definition holds `range` outside
/// their name.
std::optional<index::SymbolHash> innermost_function(llvm::ArrayRef<DefinedFunction> functions,
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

void collect_by_target(const index::IndexQuery& index,
                       index::SymbolHash hash,
                       Fid anchor,
                       RelationKind kind,
                       llvm::DenseMap<index::SymbolHash, std::vector<index::Site>>& by_target) {
    // The focus-file preamble entry cannot contribute: the preamble region
    // holds only preprocessor directives, never call or type relations.
    index.for_each_relation(hash,
                            anchor,
                            kind,
                            index::IndexQuery::Order::DiskFirst,
                            {.preamble = false},
                            [&](const index::IndexSource& source, const index::Relation& relation) {
                                if(auto site = source.site(relation.range)) {
                                    by_target[relation.target].push_back(*site);
                                }
                                return true;
                            });
}

std::vector<CallEdge>
    located_edges(const index::IndexQuery& index,
                  llvm::DenseMap<index::SymbolHash, std::vector<index::Site>>& by_target) {
    std::vector<CallEdge> result;
    result.reserve(by_target.size());
    for(auto& [target, sites]: by_target) {
        auto located = index.resolve(target, sites.front().file);
        if(!located) {
            continue;
        }
        // A relation present in both a shard and an overlay arrives twice;
        // hierarchy items must not repeat call sites.
        index::dedup_sites(sites);
        result.push_back({.symbol = std::move(*located), .sites = std::move(sites)});
    }
    return result;
}

std::vector<DefinedFunction> defined_functions(const index::IndexQuery& index,
                                               const index::IndexSource& source) {
    std::vector<DefinedFunction> functions;
    source.shard->for_each_relation([&](index::SymbolHash symbol, const index::Relation& defined) {
        if(defined.kind != RelationKind::Definition || !defined.extent().contains(defined.range)) {
            return true;
        }
        if(auto info = index.symbol_info(symbol, source.file);
           info && index::is_function(info->kind)) {
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

std::vector<CallEdge> referencing_functions(const index::IndexQuery& index,
                                            index::SymbolHash hash,
                                            Fid anchor) {
    // Each file's function definitions, by their extents.
    llvm::DenseMap<const index::Shard*, std::vector<DefinedFunction>> functions;
    llvm::DenseMap<index::SymbolHash, std::vector<index::Site>> by_function;
    auto visit = [&](const index::IndexSource& source, const index::Relation& relation) {
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
                                index::IndexQuery::Order::DiskFirst,
                                {.preamble = false},
                                visit);
    }
    return located_edges(index, by_function);
}

}  // namespace

std::vector<std::string> enclosing_functions(const index::IndexQuery& index,
                                             llvm::ArrayRef<index::Site> sites) {
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

CallGraph call_graph(const index::IndexQuery& index,
                     index::SymbolHash root,
                     Fid anchor,
                     CallGraphOptions options) {
    CallGraph graph;
    if(options.callers) {
        auto info = index.symbol_info(root, anchor);
        bool value = info && ranges::contains(std::array<SymbolKind, 3>{SymbolKind::Variable,
                                                                        SymbolKind::Field,
                                                                        SymbolKind::EnumMember},
                                              info->kind);
        if(value) {
            // A function's local has no caller but that function.
            if(info->kind != SymbolKind::Variable ||
               info->scope != index::SymbolScope::FunctionLocal) {
                graph.callers = referencing_functions(index, root, anchor);
            }
        } else {
            llvm::DenseMap<index::SymbolHash, std::vector<index::Site>> by_caller;
            collect_by_target(index, root, anchor, RelationKind::Caller, by_caller);
            // A call through a method the root overrides may dispatch to it,
            // save one naming that method by qualification.
            if(info && index::is_function(info->kind)) {
                for(auto base: index.transitive_targets(root, anchor, RelationKind::Interface)) {
                    llvm::DenseMap<index::SymbolHash, std::vector<index::Site>> through_base;
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
                            // FIXME: a qualified call and a virtual call
                            // one macro invocation spells share the
                            // invocation's site, so the qualified one hides
                            // the other and the override loses this caller.
                            if(ranges::none_of(direct, [&](const index::Site& site) {
                                   return index::covers(call, site);
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
        llvm::DenseMap<index::SymbolHash, std::vector<index::Site>> by_callee;
        collect_by_target(index, root, anchor, RelationKind::Callee, by_callee);
        graph.callees = located_edges(index, by_callee);
    }
    return graph;
}

TypeHierarchy type_hierarchy(const index::IndexQuery& index,
                             index::SymbolHash root,
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

namespace {

/// The item of a symbol at its canonical site (see call_hierarchy_items),
/// its qualified name from any project knowing it.
template <typename Item>
Item item_of(const index::PeerQueries& queries, const index::IndexQuery::Located& located) {
    auto& symbol = located.symbol;
    Item item;
    item.name = symbol.display_name();
    if(auto detail = queries.qualified_name(symbol.hash, located.site.file) + symbol.signature;
       detail != item.name) {
        item.detail = std::move(detail);
    }
    item.kind = to_protocol(symbol.kind);
    item.uri = to_uri(located.site.path);
    item.range = to_range(located.extent);
    item.selection_range = to_range(located.site);
    item.data = protocol::LSPAny(std::format("{}", symbol.hash));
    return item;
}

/// Whether two projects' answers name one entity: the same id, or the
/// same name declared at the same place — an id hashing its file differs
/// between a project compiling the file from inside its root and one
/// compiling it from outside (index::PeerQueries::Asked).
bool same_entity(const index::IndexQuery::Located& lhs, const index::IndexQuery::Located& rhs) {
    return lhs.symbol.hash == rhs.symbol.hash ||
           (lhs.symbol.name == rhs.symbol.name && lhs.site.file == rhs.site.file &&
            lhs.site.range == rhs.site.range);
}

/// Each project's neighbours of `symbol` on one side of its call graph,
/// its callers or its callees: the sites the project may answer for, one
/// neighbour per symbol, a neighbour left with no site dropped.
std::vector<CallEdge> call_edges(const index::PeerQueries& queries,
                                 index::SymbolHash symbol,
                                 Fid file,
                                 bool callers) {
    std::vector<CallEdge> into;
    auto asked = queries.asked(symbol, file);
    for(auto& [from, named]: asked) {
        auto graph = call_graph(*from, named, file, {.callers = callers, .callees = !callers});
        for(auto& edge: callers ? graph.callers : graph.callees) {
            std::erase_if(edge.sites, [&](const index::Site& site) {
                return !queries.answers_for(*from, site.file, asked);
            });
            if(edge.sites.empty()) {
                continue;
            }
            auto known = ranges::find_if(into, [&](const CallEdge& other) {
                return same_entity(other.symbol, edge.symbol);
            });
            if(known == into.end()) {
                into.push_back(std::move(edge));
            } else {
                known->sites.append_range(edge.sites);
                index::dedup_sites(known->sites);
            }
        }
    }
    return into;
}

template <typename Call>
std::vector<Call>
    calls(const index::PeerQueries& queries, index::SymbolHash symbol, Fid file, bool callers) {
    std::vector<Call> results;
    for(auto& edge: call_edges(queries, symbol, file, callers)) {
        results.push_back(
            {item_of<protocol::CallHierarchyItem>(queries, edge.symbol), to_ranges(edge.sites)});
    }
    return results;
}

/// The same for the type hierarchy, its supertypes or its subtypes, each
/// placed at one site.
std::vector<protocol::TypeHierarchyItem> type_neighbours(const index::PeerQueries& queries,
                                                         index::SymbolHash symbol,
                                                         Fid file,
                                                         bool supertypes) {
    std::vector<index::IndexQuery::Located> types;
    auto asked = queries.asked(symbol, file);
    for(auto& [from, named]: asked) {
        auto hierarchy =
            type_hierarchy(*from, named, file, {.supertypes = supertypes, .subtypes = !supertypes});
        for(auto& one: supertypes ? hierarchy.supertypes : hierarchy.subtypes) {
            if(queries.answers_for(*from, one.site.file, asked) &&
               ranges::none_of(types, [&](const index::IndexQuery::Located& other) {
                   return same_entity(other, one);
               })) {
                types.push_back(std::move(one));
            }
        }
    }
    std::vector<protocol::TypeHierarchyItem> results;
    for(auto& located: types) {
        results.push_back(item_of<protocol::TypeHierarchyItem>(queries, located));
    }
    return results;
}

}  // namespace

std::vector<protocol::CallHierarchyItem>
    call_hierarchy_items(const index::PeerQueries& queries,
                         const index::IndexQuery::Cursor& cursor) {
    std::vector<protocol::CallHierarchyItem> items;
    for(auto& located: queries.own.resolve_at(cursor)) {
        auto kind = located.symbol.kind;
        bool function = kind == SymbolKind::Function || kind == SymbolKind::Method ||
                        kind == SymbolKind::Operator;
        bool value = kind == SymbolKind::Field || kind == SymbolKind::EnumMember ||
                     (kind == SymbolKind::Variable &&
                      located.symbol.scope != index::SymbolScope::FunctionLocal);
        if(function || value) {
            items.push_back(item_of<protocol::CallHierarchyItem>(queries, located));
        }
    }
    return items;
}

std::vector<protocol::TypeHierarchyItem>
    type_hierarchy_items(const index::PeerQueries& queries,
                         const index::IndexQuery::Cursor& cursor) {
    std::vector<protocol::TypeHierarchyItem> items;
    for(auto& located: queries.own.resolve_at(cursor)) {
        auto kind = located.symbol.kind;
        if(kind == SymbolKind::Class || kind == SymbolKind::Struct || kind == SymbolKind::Enum ||
           kind == SymbolKind::Union) {
            items.push_back(item_of<protocol::TypeHierarchyItem>(queries, located));
        }
    }
    return items;
}

std::optional<index::SymbolHash> item_symbol(const index::IndexQuery& query,
                                             Fid file,
                                             std::optional<index::IndexQuery::Cursor> at_range,
                                             const std::optional<protocol::LSPAny>& data) {
    if(auto hash = data_symbol(data); hash && query.symbol_info(*hash, file)) {
        return hash;
    }
    if(at_range) {
        return at_range->symbols.front();
    }
    return std::nullopt;
}

std::vector<protocol::CallHierarchyIncomingCall> incoming_calls(const index::PeerQueries& queries,
                                                                index::SymbolHash symbol,
                                                                Fid file) {
    // TODO: callers are not capped: a function as common as llvm::errs() answers
    // thousands, each with its call ranges and a qualified name; a cap would
    // follow `[references] limit`.
    return calls<protocol::CallHierarchyIncomingCall>(queries, symbol, file, /*callers=*/true);
}

std::vector<protocol::CallHierarchyOutgoingCall> outgoing_calls(const index::PeerQueries& queries,
                                                                index::SymbolHash symbol,
                                                                Fid file) {
    return calls<protocol::CallHierarchyOutgoingCall>(queries, symbol, file, /*callers=*/false);
}

std::vector<protocol::TypeHierarchyItem> supertypes(const index::PeerQueries& queries,
                                                    index::SymbolHash symbol,
                                                    Fid file) {
    return type_neighbours(queries, symbol, file, /*supertypes=*/true);
}

std::vector<protocol::TypeHierarchyItem> subtypes(const index::PeerQueries& queries,
                                                  index::SymbolHash symbol,
                                                  Fid file) {
    return type_neighbours(queries, symbol, file, /*supertypes=*/false);
}

}  // namespace clice::feature

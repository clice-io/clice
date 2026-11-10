module clice;

import :feature.document_symbols;
import :feature.hierarchy;
import :feature.protocol;
import :index.query;
import :index.search;
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

/// The relations of `kind` the symbol has, by their targets, those written
/// in `body` alone when it is given.
void collect_by_target(const index::IndexQuery& index,
                       index::SymbolHash hash,
                       Fid anchor,
                       RelationKind kind,
                       llvm::DenseMap<index::SymbolHash, std::vector<index::Site>>& by_target,
                       const std::optional<index::Site>& body = std::nullopt) {
    // The focus-file preamble entry cannot contribute: the preamble region
    // holds only preprocessor directives, never call or type relations.
    index.for_each_relation(hash,
                            anchor,
                            kind,
                            index::IndexQuery::Order::DiskFirst,
                            {.preamble = false},
                            [&](const index::IndexSource& source, const index::Relation& relation) {
                                if(auto site = source.site(relation.range);
                                   site && (!body || index::covers(*body, *site))) {
                                    by_target[relation.target].push_back(*site);
                                }
                                return true;
                            });
}

/// The places a symbol stands at in a call graph: its canonical site, or
/// each of its definitions when it has several — a function several files
/// define (a weak and a strong one, a `main` per program) is one symbol
/// with a body in each; none when no source places it.
std::vector<index::IndexQuery::Located> placements(const index::IndexQuery& index,
                                                   index::SymbolHash hash,
                                                   Fid anchor) {
    auto canonical = index.resolve(hash, anchor);
    if(!canonical) {
        return {};
    }
    auto definitions = index.definitions(hash, anchor);
    if(definitions.size() < 2) {
        return {std::move(*canonical)};
    }
    return ranges::to<std::vector>(definitions |
                                   views::transform([&](index::IndexQuery::Placed& placed) {
                                       return index::IndexQuery::Located{
                                           .symbol = canonical->symbol,
                                           .site = std::move(placed.site),
                                           .extent = std::move(placed.extent),
                                       };
                                   }));
}

/// Each caller at the definition its calls are written in.
std::vector<CallEdge>
    located_callers(const index::IndexQuery& index,
                    llvm::DenseMap<index::SymbolHash, std::vector<index::Site>>& by_caller) {
    std::vector<CallEdge> result;
    for(auto& [caller, sites]: by_caller) {
        auto places = placements(index, caller, sites.front().file);
        if(places.empty()) {
            continue;
        }
        // A relation present in both a shard and an overlay arrives twice;
        // hierarchy items must not repeat call sites.
        index::dedup_sites(sites);
        std::vector<std::vector<index::Site>> calls(places.size());
        for(auto& site: sites) {
            auto holder = ranges::find_if(places, [&](const index::IndexQuery::Located& place) {
                return index::covers(place.extent, site);
            });
            calls[holder != places.end() ? holder - places.begin() : 0].push_back(std::move(site));
        }
        for(auto&& [place, written]: views::zip(places, calls)) {
            if(!written.empty()) {
                result.push_back({
                    .symbol = std::move(place),
                    .sites = std::move(written),
                    .one_of_several = places.size() > 1,
                });
            }
        }
    }
    return result;
}

/// Each callee at each of its places, with every call to it: a call may
/// reach any body of a function several files define.
std::vector<CallEdge>
    located_callees(const index::IndexQuery& index,
                    llvm::DenseMap<index::SymbolHash, std::vector<index::Site>>& by_callee) {
    std::vector<CallEdge> result;
    for(auto& [callee, sites]: by_callee) {
        index::dedup_sites(sites);
        auto places = placements(index, callee, sites.front().file);
        for(auto& place: places) {
            result.push_back({
                .symbol = std::move(place),
                .sites = sites,
                .one_of_several = places.size() > 1,
            });
        }
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
    for(auto kind: {RelationKind::Reference, RelationKind::Candidate, RelationKind::Guess}) {
        index.for_each_relation(hash,
                                anchor,
                                kind,
                                index::IndexQuery::Order::DiskFirst,
                                {.preamble = false},
                                visit);
    }
    return located_callers(index, by_function);
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
            // The calls to `symbol` a site of `mark` covers, or those none
            // covers.
            auto add_calls =
                [&](index::SymbolHash symbol, Fid from, RelationKind mark, bool covered) {
                    llvm::DenseMap<index::SymbolHash, std::vector<index::Site>> through;
                    collect_by_target(index, symbol, from, RelationKind::Caller, through);
                    auto marks = index.sites(symbol, from, mark);
                    for(auto& [caller, calls]: through) {
                        for(auto& call: calls) {
                            // A call a macro spells sits at the whole
                            // invocation, the name it qualifies inside.
                            // FIXME: a qualified call and a virtual call
                            // one macro invocation spells share the
                            // invocation's site, so the qualified one hides
                            // the other and the override loses this caller.
                            if(ranges::any_of(marks, [&](const index::Site& site) {
                                   return index::covers(call, site);
                               }) == covered) {
                                by_caller[caller].push_back(std::move(call));
                            }
                        }
                    }
                };
            // A call through a method the root overrides may dispatch to it,
            // save one naming that method by qualification.
            if(info && index::is_function(info->kind)) {
                for(auto base: index.transitive_targets(root, anchor, RelationKind::Interface)) {
                    add_calls(base.symbol, base.anchor, RelationKind::Direct, false);
                }
            }
            if(info && index::name_form(info->flags) == index::NameForm::Selector) {
                for(auto& other: index::same_selector_methods(index, *info)) {
                    add_calls(other.symbol.hash, other.site.file, RelationKind::Candidate, true);
                }
            }
            graph.callers = located_callers(index, by_caller);
        }
    }
    if(options.callees) {
        llvm::DenseMap<index::SymbolHash, std::vector<index::Site>> by_callee;
        collect_by_target(index, root, anchor, RelationKind::Callee, by_callee, options.body);
        graph.callees = located_callees(index, by_callee);
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

/// A symbol's qualified name, from the asking project or else the first
/// other one that knows it: a neighbour only another project indexes
/// carries its id.
std::string known_name(const index::IndexQuery& query, index::SymbolHash symbol, Fid anchor) {
    if(auto name = query.qualified_name(symbol, anchor); !name.empty()) {
        return name;
    }
    for(auto& other: query.others()) {
        if(auto name = other.qualified_name(symbol, anchor); !name.empty()) {
            return name;
        }
    }
    return {};
}

/// The item of a symbol at its canonical site (see call_hierarchy_items),
/// its qualified name from any project knowing it.
template <typename Item>
Item item_of(const index::IndexQuery& query,
             const index::IndexQuery::Located& located,
             PositionEncoding encoding) {
    auto& symbol = located.symbol;
    Item item;
    item.name = symbol.display_name();
    if(auto detail = known_name(query, symbol.hash, located.site.file) + symbol.signature;
       detail != item.name) {
        item.detail = std::move(detail);
    }
    item.kind = to_protocol(symbol.kind);
    item.uri = to_uri(located.site.path);
    item.range = to_range(located.extent, encoding);
    item.selection_range = to_range(located.site, encoding);
    item.data = protocol::LSPAny(std::format("{}", symbol.hash));
    return item;
}

/// Whether two projects' answers name one entity: the same id, or the
/// same name declared at the same place — an id hashing its file differs
/// between a project compiling the file from inside its root and one
/// compiling it from outside (index::IndexQuery::Asked).
bool same_entity(const index::IndexQuery::Located& lhs, const index::IndexQuery::Located& rhs) {
    return lhs.symbol.hash == rhs.symbol.hash ||
           (lhs.symbol.name == rhs.symbol.name && lhs.site.file == rhs.site.file &&
            lhs.site.range == rhs.site.range);
}

/// Each project's neighbours of `symbol` on one side of its call graph,
/// its callers or the callees written in `body`: the sites the project may
/// answer for, one neighbour per place, a neighbour left with no site
/// dropped.
std::vector<CallEdge> call_edges(const index::IndexQuery& query,
                                 index::SymbolHash symbol,
                                 Fid file,
                                 bool callers,
                                 const std::optional<index::Site>& body) {
    std::vector<CallEdge> into;
    auto asked = query.asked(symbol, file);
    for(auto& [from, named]: asked) {
        auto graph =
            call_graph(*from, named, file, {.callers = callers, .callees = !callers, .body = body});
        for(auto& edge: callers ? graph.callers : graph.callees) {
            std::erase_if(edge.sites, [&](const index::Site& site) {
                return !query.answers_for(*from, site.file, asked);
            });
            if(edge.sites.empty()) {
                continue;
            }
            // Another project may place a neighbour at a declaration where
            // this one knows its definition; only two of several
            // definitions stay apart.
            auto known = ranges::find_if(into, [&](const CallEdge& other) {
                return same_entity(other.symbol, edge.symbol) &&
                       (!other.one_of_several || !edge.one_of_several ||
                        index::site_key(other.symbol.site) == index::site_key(edge.symbol.site));
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
std::vector<Call> calls(const index::IndexQuery& query,
                        index::SymbolHash symbol,
                        Fid file,
                        bool callers,
                        const std::optional<index::Site>& body,
                        PositionEncoding encoding) {
    std::vector<Call> results;
    for(auto& edge: call_edges(query, symbol, file, callers, body)) {
        results.push_back({item_of<protocol::CallHierarchyItem>(query, edge.symbol, encoding),
                           to_ranges(edge.sites, encoding)});
    }
    return results;
}

/// The same for the type hierarchy, its supertypes or its subtypes, each
/// placed at one site.
std::vector<protocol::TypeHierarchyItem> type_neighbours(const index::IndexQuery& query,
                                                         index::SymbolHash symbol,
                                                         Fid file,
                                                         bool supertypes,
                                                         PositionEncoding encoding) {
    std::vector<index::IndexQuery::Located> types;
    auto asked = query.asked(symbol, file);
    for(auto& [from, named]: asked) {
        auto hierarchy =
            type_hierarchy(*from, named, file, {.supertypes = supertypes, .subtypes = !supertypes});
        for(auto& one: supertypes ? hierarchy.supertypes : hierarchy.subtypes) {
            if(query.answers_for(*from, one.site.file, asked) &&
               ranges::none_of(types, [&](const index::IndexQuery::Located& other) {
                   return same_entity(other, one);
               })) {
                types.push_back(std::move(one));
            }
        }
    }
    std::vector<protocol::TypeHierarchyItem> results;
    for(auto& located: types) {
        results.push_back(item_of<protocol::TypeHierarchyItem>(query, located, encoding));
    }
    return results;
}

}  // namespace

std::vector<protocol::CallHierarchyItem>
    call_hierarchy_items(const index::IndexQuery& query,
                         const index::IndexQuery::Cursor& cursor,
                         PositionEncoding encoding) {
    std::vector<protocol::CallHierarchyItem> items;
    for(auto& located: query.resolve_at(cursor)) {
        auto kind = located.symbol.kind;
        bool function = kind == SymbolKind::Function || kind == SymbolKind::Method ||
                        kind == SymbolKind::Operator;
        bool value = kind == SymbolKind::Field || kind == SymbolKind::EnumMember ||
                     (kind == SymbolKind::Variable &&
                      located.symbol.scope != index::SymbolScope::FunctionLocal);
        if(!function && !value) {
            continue;
        }
        auto places = placements(query, located.symbol.hash, cursor.site.file);
        auto named = ranges::find_if(places, [&](const index::IndexQuery::Located& place) {
            return index::covers(place.site, cursor.site);
        });
        if(named != places.end()) {
            items.push_back(item_of<protocol::CallHierarchyItem>(query, *named, encoding));
            continue;
        }
        for(auto& place: places) {
            items.push_back(item_of<protocol::CallHierarchyItem>(query, place, encoding));
        }
    }
    return items;
}

std::vector<protocol::TypeHierarchyItem>
    type_hierarchy_items(const index::IndexQuery& query,
                         const index::IndexQuery::Cursor& cursor,
                         PositionEncoding encoding) {
    std::vector<protocol::TypeHierarchyItem> items;
    for(auto& located: query.resolve_at(cursor)) {
        auto kind = located.symbol.kind;
        if(kind == SymbolKind::Class || kind == SymbolKind::Struct || kind == SymbolKind::Enum ||
           kind == SymbolKind::Union || kind == SymbolKind::Protocol) {
            items.push_back(item_of<protocol::TypeHierarchyItem>(query, located, encoding));
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

std::vector<protocol::CallHierarchyIncomingCall> incoming_calls(const index::IndexQuery& query,
                                                                index::SymbolHash symbol,
                                                                Fid file,
                                                                PositionEncoding encoding) {
    // TODO: callers are not capped: a function as common as llvm::errs() answers
    // thousands, each with its call ranges and a qualified name; a cap would
    // follow `[references] limit`.
    return calls<protocol::CallHierarchyIncomingCall>(query,
                                                      symbol,
                                                      file,
                                                      /*callers=*/true,
                                                      std::nullopt,
                                                      encoding);
}

std::vector<protocol::CallHierarchyOutgoingCall>
    outgoing_calls(const index::IndexQuery& query,
                   index::SymbolHash symbol,
                   Fid file,
                   const std::optional<index::Site>& name,
                   PositionEncoding encoding) {
    std::optional<index::Site> body;
    if(auto definitions = query.definitions(symbol, file); name && definitions.size() > 1) {
        for(auto& definition: definitions) {
            if(index::covers(definition.site, *name)) {
                body = std::move(definition.extent);
            }
        }
    }
    return calls<protocol::CallHierarchyOutgoingCall>(query,
                                                      symbol,
                                                      file,
                                                      /*callers=*/false,
                                                      body,
                                                      encoding);
}

std::vector<protocol::TypeHierarchyItem> supertypes(const index::IndexQuery& query,
                                                    index::SymbolHash symbol,
                                                    Fid file,
                                                    PositionEncoding encoding) {
    return type_neighbours(query, symbol, file, /*supertypes=*/true, encoding);
}

std::vector<protocol::TypeHierarchyItem> subtypes(const index::IndexQuery& query,
                                                  index::SymbolHash symbol,
                                                  Fid file,
                                                  PositionEncoding encoding) {
    return type_neighbours(query, symbol, file, /*supertypes=*/false, encoding);
}

}  // namespace clice::feature

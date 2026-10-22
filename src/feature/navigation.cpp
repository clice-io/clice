module;

#include "modules/prelude.h"

#include "support/logging.macros.h"

module clice;

import :feature.navigation;
import :feature.protocol;
import :index.query;
import :index.search;
import :index.symbol_query;
import :support.logging;
import :support.timer;

namespace clice::feature {

namespace {

bool is_class(SymbolKind kind) {
    return std::ranges::contains(
        std::array<SymbolKind, 3>{SymbolKind::Class, SymbolKind::Struct, SymbolKind::Union},
        kind);
}

/// Drop the cursor's own site from an answer set — standing on a
/// declaration or definition navigates to the other sites — unless it is
/// the only site the symbol has (an inline definition, nowhere else to go).
void drop_cursor_site(std::vector<index::Site>& sites, const index::Site& cursor) {
    if(sites.size() > 1) {
        std::erase_if(sites, [&](const index::Site& site) { return index::covers(site, cursor); });
    }
}

std::vector<index::Site> mismatched_declarations(const index::IndexQuery& index,
                                                 index::SymbolHash hash,
                                                 Fid anchor) {
    // A template or a system header's function is defined where it is
    // declared: no edit of the project's leaves its declaration behind.
    constexpr auto in_place = index::SymbolFlags::Template | index::SymbolFlags::SystemHeader;
    auto info = index.symbol_info(hash, anchor);
    if(!info || !index::is_function(info->kind) || index::has_flag(info->flags, in_place)) {
        return {};
    }
    auto defined = index.sites(hash, anchor, RelationKind::Definition);
    if(defined.empty()) {
        return {};
    }
    // A method defined in its class never had a declaration apart.
    if(auto parent = index.symbol_info(info->parent, anchor); parent && is_class(parent->kind)) {
        bool in_class = false;
        index.for_each_relation(
            info->parent,
            anchor,
            RelationKind::Definition,
            index::IndexQuery::Order::DiskFirst,
            {},
            [&](const index::IndexSource& source, const index::Relation& relation) {
                auto extent = index::extent_of(source, relation);
                in_class = extent && std::ranges::any_of(defined, [&](const index::Site& site) {
                               return index::covers(*extent, site);
                           });
                return !in_class;
            });
        if(in_class) {
            return {};
        }
    }
    index::SymbolQuery query{.mode = index::SymbolQuery::Mode::Exact,
                             .absolute = true,
                             .pattern = info->name};
    for(auto& container: index.container_chain(hash, anchor)) {
        query.scope.push_back({.name = container.name});
    }
    std::vector<index::Site> result;
    auto anonymous = index::has_flag(info->flags, index::SymbolFlags::AnonymousNamespace);
    for(auto& namesake: index::locate(index, query)) {
        auto& other = namesake.symbol;
        if(other.hash == hash || other.name != info->name || !index::is_function(other.kind) ||
           other.scope != info->scope || index::has_flag(other.flags, in_place) ||
           index::has_flag(other.flags, index::SymbolFlags::AnonymousNamespace) != anonymous ||
           !index.sites(other.hash, namesake.site.file, RelationKind::Definition).empty()) {
            continue;
        }
        for(auto& site: index.sites(other.hash, namesake.site.file, RelationKind::Declaration)) {
            // Two files' internal functions are never one.
            if(info->scope == index::SymbolScope::External ||
               std::ranges::any_of(defined,
                                   [&](const index::Site& own) { return own.path == site.path; })) {
                result.push_back(std::move(site));
            }
        }
    }
    index::dedup_sites(result);
    return result;
}

/// The first block of a namespace in `file`.
std::optional<index::Site> first_block(const index::IndexQuery& index,
                                       index::SymbolHash hash,
                                       Fid file) {
    auto source = index.serving(file);
    if(!source) {
        return std::nullopt;
    }
    std::optional<index::Site> first;
    source->shard->relations_of(hash,
                                RelationKind::Definition,
                                [&](const index::Relation& relation) {
                                    if(!first || relation.range.begin < first->range.begin) {
                                        if(auto site = source->site(relation.range)) {
                                            first = std::move(*site);
                                        }
                                    }
                                    return true;
                                });
    return first;
}

}  // namespace

std::vector<Highlight>
    highlights(llvm::ArrayRef<const index::Shard*> shards,
               llvm::ArrayRef<const index::InstantiationShard*> instantiations,
               llvm::ArrayRef<index::SymbolHash> symbols,
               llvm::function_ref<std::optional<SymbolKind>(index::SymbolHash)> kind_of) {
    std::vector<Highlight> result;
    auto add = [&](index::SymbolHash symbol, RelationKind relation, HighlightKind kind) {
        for(auto* shard: shards) {
            shard->relations_of(symbol, relation, [&](const index::Relation& found) {
                result.push_back({.range = found.range, .kind = kind});
                return true;
            });
        }
    };
    for(auto symbol: symbols) {
        auto kind = kind_of(symbol);
        // Only a variable is read or written; any other symbol is named.
        bool accessed =
            kind && std::ranges::contains(std::array<SymbolKind, 3>{SymbolKind::Variable,
                                                                    SymbolKind::Field,
                                                                    SymbolKind::Parameter},
                                          *kind);
        auto used = accessed ? HighlightKind::Read : HighlightKind::Text;
        add(symbol, RelationKind::Declaration, HighlightKind::Text);
        add(symbol, RelationKind::Definition, HighlightKind::Text);
        add(symbol, RelationKind::Reference, used);
        add(symbol, RelationKind::WeakReference, used);
        if(accessed) {
            add(symbol, RelationKind::Write, HighlightKind::Write);
        }
        // Its constructors are named by the class's name; the name in a
        // destructor's already refers to the class.
        if(kind && is_class(*kind)) {
            llvm::SmallVector<index::SymbolHash> constructors;
            for(auto* shard: shards) {
                shard->relations_of(symbol,
                                    RelationKind::Constructor,
                                    [&](const index::Relation& found) {
                                        constructors.push_back(found.target);
                                        return true;
                                    });
            }
            for(auto constructor: constructors) {
                add(constructor, RelationKind::Declaration, HighlightKind::Text);
                add(constructor, RelationKind::Definition, HighlightKind::Text);
            }
        }
    }
    for(auto* shard: instantiations) {
        for(auto symbol: symbols) {
            shard->records_of(symbol, [&](const index::InstantiationRecord& record) {
                if(record.target == symbol && (record.kind == index::InstantiationKind::Reference ||
                                               record.kind == index::InstantiationKind::Base)) {
                    result.push_back({.range = record.range, .kind = HighlightKind::Text});
                }
                return true;
            });
        }
    }
    // The strongest kind of a range sorts first and survives the dedup.
    std::ranges::sort(result, [](const Highlight& lhs, const Highlight& rhs) {
        return std::tuple(lhs.range.begin, lhs.range.end, rhs.kind) <
               std::tuple(rhs.range.begin, rhs.range.end, lhs.kind);
    });
    auto duplicates = std::ranges::unique(result, {}, &Highlight::range);
    result.erase(duplicates.begin(), duplicates.end());
    return result;
}

std::vector<index::Site> definition_sites(const index::IndexQuery& index,
                                          index::SymbolHash hash,
                                          Fid anchor) {
    auto info = index.symbol_info(hash, anchor);
    if(!info || info->kind != SymbolKind::Namespace) {
        return index.sites(hash, anchor, RelationKind::Definition);
    }
    // The first block in the file the table names for the namespace stands
    // for every block, whichever files are open.
    if(auto identity = index.project().find_symbol(hash);
       identity && identity->file != index::no_file) {
        if(auto first = first_block(index, hash, Fid{identity->file})) {
            return {std::move(*first)};
        }
    }
    if(auto placed = index.canonical_placed(hash, anchor)) {
        return {std::move(placed->site)};
    }
    return {};
}

std::vector<index::Site> definition(const index::IndexQuery& index,
                                    const index::IndexQuery::Cursor& cursor) {
    ScopedTimer timer;
    auto anchor = cursor.site.file;
    std::vector<index::Site> result;
    for(auto symbol: cursor.symbols) {
        auto info = index.symbol_info(symbol, anchor);
        auto found = definition_sites(index, symbol, anchor);
        // Every block of a namespace defines it; definition gives the one.
        if(info && info->kind == SymbolKind::Namespace) {
            result.append_range(std::move(found));
            continue;
        }
        // At its own declaration, a virtual function nothing defines, a pure
        // one, is defined by its overrides; a use of it still reaches the
        // declaration.
        auto at_declaration = [&] {
            return std::ranges::any_of(
                index.sites(symbol, anchor, RelationKind::Declaration),
                [&](const index::Site& site) { return index::covers(site, cursor.site); });
        };
        if(found.empty() && info && index::is_function(info->kind) && at_declaration()) {
            for(auto target:
                index.transitive_targets(symbol, anchor, RelationKind::Implementation)) {
                if(auto located = index.resolve(target.symbol, target.anchor)) {
                    found.push_back(std::move(located->site));
                }
            }
        }
        if(found.empty() || std::ranges::any_of(found, [&](const index::Site& site) {
               return index::covers(site, cursor.site);
           })) {
            found.append_range(index.sites(symbol, anchor, RelationKind::Declaration));
            // A specialization leads on to the template it specializes.
            for(auto& primary: index.located_targets(symbol, anchor, RelationKind::Primary)) {
                found.push_back(std::move(primary.site));
            }
            index::dedup_sites(found);
            drop_cursor_site(found, cursor.site);
        }
        result.append_range(std::move(found));
    }
    index::dedup_sites(result);
    LOG_PERF("index_query",
             "kind=definition path={} results={} elapsed_ms={:.2f}",
             cursor.site.path,
             result.size(),
             timer.ms_f());
    return result;
}

std::vector<index::Site> declaration(const index::IndexQuery& index,
                                     const index::IndexQuery::Cursor& cursor) {
    ScopedTimer timer;
    std::vector<index::Site> result;
    for(auto symbol: cursor.symbols) {
        // Every block of a namespace declares it: the one definition gives,
        // and the first in the cursor's file.
        if(auto info = index.symbol_info(symbol, cursor.site.file);
           info && info->kind == SymbolKind::Namespace) {
            result.append_range(definition_sites(index, symbol, cursor.site.file));
            if(auto own = first_block(index, symbol, cursor.site.file)) {
                result.push_back(std::move(*own));
            }
            continue;
        }
        for(auto kind: {RelationKind::Declaration, RelationKind::Definition}) {
            result.append_range(index.sites(symbol, cursor.site.file, kind));
        }
    }
    index::dedup_sites(result);
    drop_cursor_site(result, cursor.site);
    // Only the cursor's own site is left when the symbol has no other.
    if(std::ranges::all_of(result, [&](const index::Site& site) {
           return index::covers(site, cursor.site);
       })) {
        std::vector<index::Site> mismatched;
        for(auto symbol: cursor.symbols) {
            mismatched.append_range(mismatched_declarations(index, symbol, cursor.site.file));
        }
        if(!mismatched.empty()) {
            result = std::move(mismatched);
        }
    }
    LOG_PERF("index_query",
             "kind=declaration path={} results={} elapsed_ms={:.2f}",
             cursor.site.path,
             result.size(),
             timer.ms_f());
    return result;
}

std::vector<index::Site> references(const index::IndexQuery& index,
                                    const index::IndexQuery::Cursor& cursor,
                                    bool include_declaration) {
    ScopedTimer timer;
    llvm::SmallVector<RelationKind, 4> kinds{RelationKind::Reference, RelationKind::WeakReference};
    if(include_declaration) {
        kinds.append({RelationKind::Declaration, RelationKind::Definition});
    }
    auto anchor = cursor.site.file;
    std::vector<index::Site> result;
    auto add = [&](index::SymbolHash symbol, Fid from, llvm::ArrayRef<RelationKind> kinds) {
        for(auto kind: kinds) {
            result.append_range(index.sites(symbol, from, kind));
        }
    };
    for(auto symbol: cursor.symbols) {
        add(symbol, anchor, kinds);
        auto info = index.symbol_info(symbol, anchor);
        if(!info) {
            continue;
        }
        // A call through a method it overrides may dispatch to it, save one
        // naming that method by qualification.
        if(index::is_function(info->kind)) {
            for(auto base: index.transitive_targets(symbol, anchor, RelationKind::Interface)) {
                auto direct = index.sites(base.symbol, base.anchor, RelationKind::Direct);
                for(auto kind: {RelationKind::Reference, RelationKind::WeakReference}) {
                    for(auto& site: index.sites(base.symbol, base.anchor, kind)) {
                        if(std::ranges::none_of(direct, [&](const index::Site& call) {
                               return index::site_key(call) == index::site_key(site);
                           })) {
                            result.push_back(std::move(site));
                        }
                    }
                }
            }
        }
        // Its constructors are named by the class's name; the name in a
        // destructor's already refers to the class.
        if(is_class(info->kind)) {
            for(auto constructor: index.targets(symbol, anchor, RelationKind::Constructor)) {
                add(constructor.symbol,
                    constructor.anchor,
                    {RelationKind::Declaration, RelationKind::Definition});
            }
        }
    }
    index::dedup_sites(result);
    LOG_PERF("index_query",
             "kind=references path={} results={} elapsed_ms={:.2f}",
             cursor.site.path,
             result.size(),
             timer.ms_f());
    return result;
}

std::vector<PlacedHighlight> document_highlights(const index::IndexQuery& index,
                                                 const index::IndexQuery::Cursor& cursor) {
    auto source = index.serving(cursor.site.file);
    if(!source) {
        return {};
    }
    std::vector<PlacedHighlight> result;
    auto kind_of = [&](index::SymbolHash hash) -> std::optional<SymbolKind> {
        if(auto info = index.symbol_info(hash, cursor.site.file)) {
            return info->kind;
        }
        return std::nullopt;
    };
    auto place = [&](llvm::ArrayRef<const index::Shard*> shards) {
        for(auto& highlight: highlights(shards, source->instantiations, cursor.symbols, kind_of)) {
            if(auto site = source->site(highlight.range)) {
                result.push_back({.site = std::move(*site), .kind = highlight.kind});
            }
        }
    };
    if(!index.with_preamble(*source, [&](const index::Shard& preamble) {
           place({source->shard, &preamble});
       })) {
        place({source->shard});
    }
    return result;
}

std::vector<index::Site> type_definition(const index::IndexQuery& index,
                                         const index::IndexQuery::Cursor& cursor,
                                         index::SymbolHash symbol) {
    auto anchor = cursor.site.file;
    llvm::SmallVector<index::IndexQuery::Target> found;
    if(auto source = index.serving(anchor)) {
        source->shard->relations_of(symbol,
                                    RelationKind::TypeDefinition,
                                    [&](const index::Relation& relation) {
                                        if(relation.range.contains(cursor.site.range)) {
                                            found.push_back(
                                                {.symbol = relation.target, .anchor = anchor});
                                        }
                                        return true;
                                    });
    }
    auto declared_type = [&](const index::IndexQuery::Target& of) {
        llvm::SmallVector<index::IndexQuery::Target> types;
        llvm::DenseSet<index::SymbolHash> seen;
        index.for_each_relation(
            of.symbol,
            of.anchor,
            RelationKind::TypeDefinition,
            index::IndexQuery::Order::DiskFirst,
            {.preamble = false},
            [&](const index::IndexSource& source, const index::Relation& relation) {
                if(!relation.range.valid() && seen.insert(relation.target).second) {
                    types.push_back({.symbol = relation.target, .anchor = source.file});
                }
                return true;
            });
        return types;
    };
    if(found.empty()) {
        // A type is its own type definition.
        if(auto info = index.symbol_info(symbol, anchor);
           info && (is_class(info->kind) || info->kind == SymbolKind::Enum ||
                    info->kind == SymbolKind::Type)) {
            found.push_back({.symbol = symbol, .anchor = anchor});
        } else {
            found = declared_type({.symbol = symbol, .anchor = anchor});
        }
    }
    // An alias leads on to the type it names, through aliases of aliases.
    llvm::DenseSet<index::SymbolHash> seen;
    for(std::uint32_t i = 0; i < found.size(); i += 1) {
        auto target = found[i];
        auto named = index.symbol_info(target.symbol, target.anchor);
        if(!seen.insert(target.symbol).second || !named || named->kind != SymbolKind::Type ||
           index::has_flag(named->flags, index::SymbolFlags::TemplateParameter)) {
            continue;
        }
        found.append(declared_type(target));
    }
    std::vector<index::Site> result;
    for(auto target: found) {
        if(auto located = index.resolve(target.symbol, target.anchor)) {
            result.push_back(std::move(located->site));
        }
    }
    index::dedup_sites(result);
    drop_cursor_site(result, cursor.site);
    return result;
}

bool names_weakly(const index::IndexQuery& index,
                  const index::IndexQuery::Cursor& cursor,
                  index::SymbolHash symbol) {
    auto source = index.serving(cursor.site.file);
    if(!source) {
        return false;
    }
    bool found = false;
    auto hit = [&](const index::Shard& shard) {
        shard.relations_of(symbol,
                           RelationKind::WeakReference,
                           [&](const index::Relation& relation) {
                               found = relation.range == cursor.site.range;
                               return !found;
                           });
        return found;
    };
    if(!hit(*source->shard)) {
        index.with_preamble(*source, [&](const index::Shard& preamble) { hit(preamble); });
    }
    return found;
}

std::vector<index::IndexQuery::Located> implementation(const index::IndexQuery& index,
                                                       index::SymbolHash hash,
                                                       Fid anchor) {
    auto info = index.symbol_info(hash, anchor);
    if(!info) {
        return {};
    }
    // Every class deriving from a class, every override of a function, at
    // any depth.
    auto kind = is_class(info->kind) ? RelationKind::Derived : RelationKind::Implementation;
    std::vector<index::IndexQuery::Located> result;
    for(auto target: index.transitive_targets(hash, anchor, kind)) {
        if(auto located = index.resolve(target.symbol, target.anchor)) {
            result.push_back(std::move(*located));
        }
    }
    // A function nothing overrides is implemented by its own definition.
    if(result.empty() && index::is_function(info->kind)) {
        index.for_each_relation(
            hash,
            anchor,
            RelationKind::Definition,
            index::IndexQuery::Order::DiskFirst,
            {},
            [&](const index::IndexSource& source, const index::Relation& relation) {
                if(auto site = source.site(relation.range)) {
                    result.push_back(
                        {.symbol = *info,
                         .site = *site,
                         .extent = index::extent_of(source, relation).value_or(*site)});
                }
                return true;
            });
    }
    return result;
}

std::vector<protocol::DocumentHighlight>
    document_highlights_to_protocol(llvm::ArrayRef<PlacedHighlight> highlights) {
    auto kind_of = [](HighlightKind kind) {
        switch(kind) {
            case HighlightKind::Text: return protocol::DocumentHighlightKind::Text;
            case HighlightKind::Read: return protocol::DocumentHighlightKind::Read;
            case HighlightKind::Write: return protocol::DocumentHighlightKind::Write;
        }
        std::unreachable();
    };
    std::vector<protocol::DocumentHighlight> result;
    result.reserve(highlights.size());
    for(const auto& highlight: highlights) {
        result.push_back({.range = to_range(highlight.site), .kind = kind_of(highlight.kind)});
    }
    return result;
}

}  // namespace clice::feature

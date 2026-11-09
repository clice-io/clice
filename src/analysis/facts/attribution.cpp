module clice;

import :analysis.facts;
import :analysis.facts.builder;
import :command.file_kind;
import :index.project_index;
import :syntax.token;

namespace clice::analysis {

std::uint32_t FactsBuilder::owner_of(index::SymbolHash hash,
                                     const index::SymbolView& identity,
                                     llvm::ArrayRef<Site> scoped,
                                     Fid canonical) {
    auto kind = identity.kind;
    // Canonically declared outside the scope. A fragment pasted into a
    // scoped header declares what that header provides; a source there
    // only implements the functions a scoped header declares; any other
    // file provides the entity itself, and a scoped declaration of it
    // would redeclare it under the wrong module.
    auto owner = none;
    if(canonical.valid() && id_of(canonical) == none) {
        if(is_context_header_path(llvm::StringRef(table.resolve(canonical)))) {
            for(auto includer: included_by.lookup(canonical)) {
                auto id = id_of(includer);
                if(id != none &&
                   (owner == none || facts.files[id].path < facts.files[owner].path)) {
                    owner = id;
                }
            }
        }
        bool implemented = index.indexed(canonical) &&
                           !ranges::contains(std::array<SymbolKind, 6>{SymbolKind::Class,
                                                                       SymbolKind::Struct,
                                                                       SymbolKind::Union,
                                                                       SymbolKind::Enum,
                                                                       SymbolKind::Type,
                                                                       SymbolKind::Concept},
                                             kind);
        if(owner == none && !implemented) {
            if(kind == SymbolKind::Macro) {
                foreign_macros[hash] = {canonical, identity.name.str()};
                return none;
            }
            for(auto& site: scoped) {
                if(!site.definition) {
                    facts.foreign_declarations.push_back({
                        .name = qualified_name(hash, index.shard(fids[site.file])),
                        .owner = display(canonical),
                        .file = site.file,
                        .line = site.line,
                        .friend_declaration = writes(site.file, site.line, "friend"),
                    });
                }
            }
            return none;
        }
    }
    if(owner == none && scoped.empty()) {
        return none;
    }

    auto pick = [&](bool header, bool definition) {
        auto owner = none;
        for(auto& site: scoped) {
            if(facts.files[site.file].source == header || site.definition != definition) {
                continue;
            }
            if(owner == none || facts.files[site.file].path < facts.files[owner].path) {
                owner = site.file;
            }
        }
        return owner;
    };
    constexpr std::pair<bool, bool> preference[] = {
        {true,  true },
        {true,  false},
        {false, true },
        {false, false},
    };
    for(auto [header, definition]: preference) {
        if(owner == none) {
            owner = pick(header, definition);
        }
        // Defined in no header, it belongs to the header of its first
        // declaration, not to whichever forward declaration sorts first.
        if(owner == none && header && definition && canonical.valid()) {
            if(auto id = id_of(canonical); id != none && !facts.files[id].source) {
                owner = id;
            }
        }
    }
    return owner;
}

std::uint32_t FactsBuilder::paster_of(std::uint32_t fragment,
                                      index::SymbolHash hash,
                                      const index::SymbolView& identity,
                                      llvm::ArrayRef<Site> scoped) {
    // A fragment's entities belong to the file pasting it in: the one
    // whose declaration encloses the paste when the fragment fills a
    // declaration (clang's Options.inc filling clang's enum in clang and
    // the program's enum in the program), else the first by path.
    auto includers = facts.including_files(fragment, true);
    if(includers.empty()) {
        return none;
    }
    // A site in a fragment stands for the files including it.
    auto stand_for = [&](std::uint32_t file, std::uint32_t includer) {
        return ranges::contains(facts.including_files(file, true), includer);
    };
    auto narrow = [&](auto keeps) {
        auto kept = includers;
        kept.erase(ranges::remove_if(kept, [&](std::uint32_t includer) { return !keeps(includer); })
                       .begin(),
                   kept.end());
        if(!kept.empty()) {
            includers = std::move(kept);
        }
    };
    // A fragment its includers read differently declares an entity for the
    // includers the units whose reading declares it enter (common/omp.inc
    // declares sema_clause where sema/check.h defines GEN_SEMA ahead of it).
    if(auto units = declaring_units.lookup({fragment, hash}); !units.empty()) {
        narrow([&](std::uint32_t includer) {
            return ranges::any_of(units, [&](std::uint32_t unit) {
                return ranges::contains(facts.files[includer].units, unit);
            });
        });
    }
    if(auto enclosing = sites.find(identity.parent); enclosing != sites.end()) {
        narrow([&](std::uint32_t includer) {
            return ranges::any_of(enclosing->second,
                                  [&](const Site& site) { return stand_for(site.file, includer); });
        });
    }
    // The includer defining the X-macro the table's line expands, a
    // level deep (OMPConstants.h defines OMP_DEFAULT_KIND, which
    // OMPKinds.def's __OMP_DEFAULT_KIND(none) expands to a constant).
    if(auto site = ranges::find(scoped, fragment, &Site::file);
       site != scoped.end() && includers.size() > 1) {
        llvm::StringSet<> names;
        for(const auto& token: tokens_on(fragment, site->line)) {
            if(token.is_identifier()) {
                names.insert(token.text(text_of(fragment).content));
            }
        }
        for(auto& word: ranges::to<llvm::SmallVector<llvm::StringRef>>(names.keys())) {
            for(auto [file, line]: macro_definitions.lookup(word)) {
                for_each_identifier(directive_text(file, line),
                                    [&](llvm::StringRef inner) { names.insert(inner); });
            }
        }
        narrow([&](std::uint32_t includer) {
            return ranges::any_of(names.keys(), [&](llvm::StringRef word) {
                return ranges::any_of(macro_definitions.lookup(word), [&](auto& definition) {
                    return definition.first != fragment && stand_for(definition.first, includer);
                });
            });
        });
    }
    return *ranges::min_element(includers, [&](std::uint32_t lhs, std::uint32_t rhs) {
        return facts.files[lhs].path < facts.files[rhs].path;
    });
}

void FactsBuilder::link_members() {
    for(std::uint32_t entity = 0; entity < facts.entities.size(); entity += 1) {
        auto& info = facts.entities[entity];
        llvm::DenseSet<std::uint32_t> visited{entity};
        for(auto parent = entity_of(parents[entity]);
            parent != none && facts.entities[parent].owner == info.owner &&
            visited.insert(parent).second;
            parent = entity_of(parents[parent])) {
            info.top = parent;
        }
    }
}

std::expected<void, std::string> move_entities(Facts& facts,
                                               llvm::StringRef spec,
                                               llvm::ArrayRef<index::SymbolHash> symbols,
                                               llvm::StringRef path) {
    llvm::SmallVector<std::uint32_t> selected;
    for(std::uint32_t entity = 0; entity < facts.entities.size(); entity += 1) {
        auto& info = facts.entities[entity];
        if(info.kind != SymbolKind::Macro && ranges::contains(symbols, info.hash)) {
            selected.push_back(entity);
        }
    }
    if(selected.empty()) {
        return std::unexpected(
            std::format("--move-entity {}: no entity in scope", std::string_view(spec)));
    }
    // A member moves with its class.
    for(auto& entity: selected) {
        entity = facts.entities[entity].top;
    }

    if(auto known = facts.file_ids.find(path);
       known == facts.file_ids.end() ? !is_header_path(path) : facts.files[known->second].source) {
        return std::unexpected(std::format("--move-entity {}: {} is not a header",
                                           std::string_view(spec),
                                           path.str()));
    }
    auto [it, inserted] =
        facts.file_ids.try_emplace(path, static_cast<std::uint32_t>(facts.files.size()));
    auto target = it->second;
    if(inserted) {
        facts.files.push_back({.path = path.str()});
        facts.uses.emplace_back();
        facts.macro_uses.emplace_back();
    }

    auto moves = [&](std::uint32_t entity) {
        return ranges::contains(selected, facts.entities[entity].top);
    };
    llvm::SmallVector<std::uint32_t> moved;
    for(std::uint32_t entity = 0; entity < facts.entities.size(); entity += 1) {
        if(moves(entity) && facts.entities[entity].owner != target) {
            moved.push_back(entity);
        }
    }
    auto add =
        [](std::vector<Use>& uses, std::uint32_t entity, std::uint32_t count, std::uint32_t line) {
            auto use = ranges::lower_bound(uses, entity, {}, &Use::entity);
            if(use == uses.end() || use->entity != entity) {
                use = uses.insert(use, {.entity = entity, .line = line});
            }
            use->count += count;
        };
    auto remove = [](std::vector<Use>& uses, std::uint32_t entity, std::uint32_t count) {
        auto use = ranges::lower_bound(uses, entity, {}, &Use::entity);
        assert(use != uses.end() && use->entity == entity && use->count >= count);
        use->count -= count;
        if(use->count == 0) {
            uses.erase(use);
        }
    };

    // (old owner, entity) -> the times the moved definitions name it.
    std::map<std::pair<std::uint32_t, std::uint32_t>, std::uint32_t> carried;
    for(auto entity: moved) {
        for(auto& use: facts.entities[entity].body) {
            carried[{facts.entities[entity].owner, use.entity}] += use.count;
        }
    }
    // What stays behind is named from the target instead of the old owner.
    for(auto& [key, count]: carried) {
        auto [from, entity] = key;
        auto& used = facts.entities[entity];
        if(moves(entity) && used.owner != target) {
            continue;
        }
        if(used.owner == from) {
            used.self_uses -= count;
        } else {
            remove(facts.uses[from], entity, count);
        }
        if(used.owner == target) {
            used.self_uses += count;
        } else {
            add(facts.uses[target], entity, count, 0);
        }
    }
    // A moved entity named by the moved definitions or by the target is
    // named by its owner; named elsewhere in its old owner, it is used
    // across files.
    for(auto entity: moved) {
        auto& info = facts.entities[entity];
        std::uint32_t inside = 0;
        for(auto& [key, count]: carried) {
            auto [from, used] = key;
            if(used != entity) {
                continue;
            }
            inside += count;
            if(from != info.owner) {
                remove(facts.uses[from], entity, count);
            }
        }
        // The owner's first use stays behind unless a moved definition
        // holds uses of it, whose lines no entity records apart.
        auto moving = carried[{info.owner, entity}];
        auto behind = info.self_uses - moving;
        if(behind != 0) {
            add(facts.uses[info.owner], entity, behind, moving == 0 ? info.self_line : 0);
        }
        auto& from_target = facts.uses[target];
        if(auto use = ranges::lower_bound(from_target, entity, {}, &Use::entity);
           use != from_target.end() && use->entity == entity) {
            inside += use->count;
            from_target.erase(use);
        }
        info.self_uses = inside;
        info.self_line = 0;
    }
    for(auto entity: moved) {
        facts.entities[entity].owner = target;
    }
    return {};
}

}  // namespace clice::analysis

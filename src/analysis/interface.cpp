module clice;

import :analysis.interface;
import :analysis.module_graph;
import :syntax.lexer;
import :syntax.lexical_scan;
import :syntax.token;
import :vfs.path;

namespace clice::analysis {

namespace {

/// Visit each identifier a macro's directive (Text::directive_text)
/// expands: neither the macro's own name nor its parameters, which name
/// nothing outside it.
void for_each_expanded(const std::string& directive,
                       llvm::function_ref<void(llvm::StringRef)> visit) {
    Lexer lexer(directive, raw_dialect(clang::Language::CXX, clang::LangStandard::lang_cxx23));
    llvm::SmallVector<Token, 16> words;
    lexer.advance();
    for(auto token = lexer.advance(); !token.is_eod() && !token.is_eof(); token = lexer.advance()) {
        words.push_back(token);
    }
    auto definition = macro_definition(words);
    if(!definition) {
        return;
    }
    llvm::SmallVector<llvm::StringRef> parameters;
    for(const auto& parameter: definition->parameters) {
        if(parameter.is_identifier()) {
            parameters.push_back(parameter.text(directive));
        }
    }
    for(const auto& token: definition->body) {
        if(token.is_identifier() && !ranges::contains(parameters, token.text(directive))) {
            visit(token.text(directive));
        }
    }
}

}  // namespace

std::vector<Interface> interfaces(const Facts& facts, const Partition& partition) {
    auto count = partition.modules.size();
    Reverse reverse(facts);
    auto module_of = [&](std::uint32_t file) {
        return partition.module_of[file];
    };
    // A header of a module kept headers still compiles where a user file
    // names it if the user's include chains reach it without an emptied
    // header of a wrapped module between. A header reading differently by
    // its includer (<stdarg.h> under <stdio.h>'s __need___va_list defines no
    // va_start) has its own include edges per entry: those continue from
    // the tree nodes entered under that includer. The trees hold no node for
    // a directive clang's multiple-include optimization skips unread: a
    // header no tree enters under the includer continues by its own edges.
    // What only some of its readings declare is there only where the user
    // includes it itself: the C library's <time.h> under __need_time_t lacks
    // localtime_r. What every reading declares is wherever the header is.
    std::vector<std::vector<std::vector<std::uint32_t>>> children(facts.trees.size());
    /// Per scoped file, its (tree, node) pairs, ordered.
    std::vector<std::vector<std::pair<std::uint32_t, std::uint32_t>>> nodes_of(facts.files.size());
    for(std::uint32_t tree = 0; tree < facts.trees.size(); tree += 1) {
        auto& files = facts.trees[tree].files;
        auto& parents = facts.trees[tree].parents;
        children[tree].resize(parents.size());
        for(std::uint32_t node = 0; node < parents.size(); node += 1) {
            if(parents[node] != none) {
                children[tree][parents[node]].push_back(node);
            }
            if(files[node] != none) {
                nodes_of[files[node]].emplace_back(tree, node);
            }
        }
    }
    auto contextual = [&](std::uint32_t file) {
        return facts.files[file].variants > 1;
    };
    llvm::DenseMap<std::uint32_t, llvm::DenseSet<std::uint32_t>> closures;
    auto reaches = [&](std::uint32_t user, std::uint32_t target, bool every_reading) {
        if(contextual(target) && !every_reading) {
            return user == target || ranges::contains(facts.files[user].includes, target);
        }
        auto [it, inserted] = closures.try_emplace(user);
        auto& closure = it->second;
        if(!inserted) {
            return closure.contains(target);
        }
        auto home = module_of(user);
        auto emptied = [&](std::uint32_t file) {
            return module_of(file) != home && partition.emptied(module_of(file));
        };
        llvm::SmallVector<std::uint32_t> files;
        llvm::SmallVector<std::pair<std::uint32_t, std::uint32_t>> nodes;
        llvm::DenseSet<std::pair<std::uint32_t, std::uint32_t>> seen;
        auto visit_node = [&](std::uint32_t tree, std::uint32_t node) {
            auto file = facts.trees[tree].files[node];
            if(file == none || emptied(file)) {
                return;
            }
            if(!contextual(file)) {
                files.push_back(file);
            } else if(seen.insert({tree, node}).second) {
                closure.insert(file);
                nodes.emplace_back(tree, node);
            }
        };
        if(contextual(user)) {
            for(auto [tree, node]: nodes_of[user]) {
                visit_node(tree, node);
            }
        } else {
            files.push_back(user);
        }
        while(!files.empty() || !nodes.empty()) {
            if(!files.empty()) {
                auto current = files.pop_back_val();
                if(!closure.insert(current).second) {
                    continue;
                }
                for(auto included: facts.files[current].includes) {
                    if(emptied(included)) {
                        continue;
                    }
                    auto entered = false;
                    if(contextual(included)) {
                        for(auto [tree, node]: nodes_of[included]) {
                            auto parent = facts.trees[tree].parents[node];
                            if(parent != none && facts.trees[tree].files[parent] == current &&
                               !facts.trees[tree].skipped[node]) {
                                visit_node(tree, node);
                                entered = true;
                            }
                        }
                    }
                    if(!entered) {
                        files.push_back(included);
                    }
                }
                continue;
            }
            auto [tree, node] = nodes.pop_back_val();
            for(auto child: children[tree][node]) {
                visit_node(tree, child);
            }
        }
        return closure.contains(target);
    };

    std::vector<std::set<std::uint32_t>> imports(count), entries(count), textual(count),
        reads(count);
    /// A kept module's textual header -> an (entity, user file) needing it.
    std::vector<llvm::DenseMap<std::uint32_t, std::pair<std::uint32_t, std::uint32_t>>> because(
        count);
    /// A kept module's names its unit exports, and its headers holding an
    /// internal-linkage entity of those files of other modules name.
    std::vector<std::set<std::tuple<std::string, std::uint32_t, ExportForm>>> kept_exports(count);
    std::vector<std::set<std::uint32_t>> locals(count);
    std::vector<std::map<std::tuple<std::string, std::uint32_t, ExportForm>, bool>> exports(count);
    /// Per header, the modules it names or includes and the macros of other
    /// modules it reads; a fragment's are its includers'.
    std::vector<std::set<std::uint32_t>> file_imports(facts.files.size()),
        file_reads(facts.files.size());

    for(std::uint32_t file = 0; file < facts.files.size(); file += 1) {
        // A fragment's names are its includers'; the sources implementing
        // a module are no part of its interface.
        for(auto charged: facts.charged_files(file)) {
            if(facts.files[charged].source) {
                continue;
            }
            for(auto& use: facts.uses[file]) {
                file_imports[charged].insert(module_of(facts.entities[use.entity].owner));
            }
            // The program's switches a library header reads are replayed
            // ahead of its entries, not imported.
            for(auto& use: facts.macro_uses[file]) {
                auto owner = module_of(facts.entities[use.entity].owner);
                if(partition.kinds[owner] != ModuleKind::Program) {
                    file_imports[charged].insert(owner);
                }
                if(owner != module_of(charged) && !facts.entities[use.entity].undefined) {
                    file_reads[charged].insert(use.entity);
                }
            }
        }
        if(facts.files[file].source) {
            continue;
        }
        auto module = module_of(file);
        for(auto included: facts.files[file].includes) {
            file_imports[file].insert(module_of(included));
        }
        if(!facts.files[file].fragment &&
           ranges::any_of(facts.files[file].includers,
                          [&](std::uint32_t includer) { return module_of(includer) != module; })) {
            entries[module].insert(file);
        }
    }

    // A file declaring a function itself, or defining what it uses, needs no
    // kept header for it: toml++ declares the Windows functions it calls, in
    // a fragment its header pastes.
    llvm::DenseSet<std::pair<std::uint32_t, std::uint32_t>> self_declared;
    for(auto& redeclaration: facts.redeclarations) {
        auto kind = facts.entities[redeclaration.entity].kind;
        if(!redeclaration.friend_declaration &&
           (redeclaration.definition || kind == SymbolKind::Function)) {
            for(auto file: facts.charged_files(redeclaration.file)) {
                self_declared.insert({redeclaration.entity, file});
            }
        }
    }

    // Macros by name per module, for the closure of their directives.
    std::vector<llvm::StringMap<llvm::SmallVector<std::uint32_t, 1>>> macros_named(count);
    std::vector<std::set<std::uint32_t>> macros(count);
    for(std::uint32_t entity = 0; entity < facts.entities.size(); entity += 1) {
        auto& info = facts.entities[entity];
        auto module = module_of(info.owner);
        auto kept = partition.kinds[module] == ModuleKind::Textual;
        auto& users = reverse.users[entity];
        auto user = ranges::find_if(users, [&](std::uint32_t candidate) {
            return module_of(candidate) != module &&
                   partition.kinds[module_of(candidate)] != ModuleKind::External &&
                   (!kept || (!self_declared.contains({entity, candidate}) &&
                              !reaches(candidate, info.owner, !info.partial)));
        });
        auto outside = user != users.end();
        if(info.kind == SymbolKind::Macro) {
            if(info.undefined) {
                continue;
            }
            macros_named[module][info.name].push_back(entity);
            if(outside) {
                macros[module].insert(entity);
            }
        } else if(facts.files[info.owner].source || info.export_name.empty()) {
            continue;
        } else if(info.linkage == InternalLinkage::None) {
            // A kept module's name no imported module provides comes with
            // its header.
            if(kept && outside && !partition.provides[module].contains(info.export_name)) {
                textual[module].insert(info.owner);
                because[module].try_emplace(info.owner, entity, *user);
                auto& enumeration = facts.entities[info.top];
                if(info.kind != SymbolKind::EnumMember || enumeration.kind != SymbolKind::Enum) {
                    kept_exports[module].insert({info.export_name, info.owner, ExportForm::Name});
                } else if(!enumeration.export_name.empty()) {
                    kept_exports[module].insert(
                        {enumeration.export_name, info.owner, ExportForm::Enum});
                } else {
                    kept_exports[module].insert({info.export_name, info.owner, ExportForm::EnumOf});
                }
            }
        } else if(outside || (!kept && info.kind == SymbolKind::Variable && !info.constant &&
                              info.linkage == InternalLinkage::Static &&
                              entries[module].contains(info.owner))) {
            // A static variable in a header other modules include is a copy
            // per includer, initialized in each: a force-linking anchor's
            // point.
            textual[module].insert(info.owner);
            if(kept) {
                because[module].try_emplace(info.owner, entity, *user);
                locals[module].insert(info.owner);
            }
        }
    }

    // A macro another module's directive spells (clang's OPTION expanding
    // llvm's LLVM_MAKE_OPT_ID_WITH_ID_PREFIX) is used where that macro is
    // expanded — also by the fragments its own header pastes, which no
    // other file's uses show (StmtVisitor.h's STMT in StmtNodes.inc); one a
    // textual header expands is used wherever that header compiles, in
    // importers.
    for(std::uint32_t entity = 0; entity < facts.entities.size(); entity += 1) {
        auto& info = facts.entities[entity];
        if(info.kind != SymbolKind::Macro) {
            continue;
        }
        auto module = module_of(info.owner);
        for_each_expanded(info.directive, [&](llvm::StringRef word) {
            for(std::uint32_t other = 0; other < count; other += 1) {
                if(other != module) {
                    for(auto entity: macros_named[other].lookup(word)) {
                        macros[other].insert(entity);
                    }
                }
            }
        });
    }
    for(std::uint32_t module = 0; module < count; module += 1) {
        if(partition.kinds[module] == ModuleKind::Textual) {
            continue;
        }
        for(auto header: textual[module]) {
            for(auto& use: facts.macro_uses[header]) {
                if(module_of(facts.entities[use.entity].owner) == module &&
                   !facts.entities[use.entity].undefined) {
                    macros[module].insert(use.entity);
                }
            }
        }
    }

    // A textual header is included where it is used, not in the fragment.
    // The fragment exports what its entries reach in the units of other
    // modules: a library's own sources enter headers its interface never
    // includes there (spdlog's -inl.h files under SPDLOG_COMPILED_LIB,
    // simdjson's other implementations, CRoaring's containers).
    std::vector<llvm::DenseSet<std::uint32_t>> reached(count);
    for(std::uint32_t module = 0; module < count; module += 1) {
        for(auto header: textual[module]) {
            entries[module].erase(header);
        }
        llvm::SmallVector<std::uint32_t> pending(entries[module].begin(), entries[module].end());
        while(!pending.empty()) {
            auto file = pending.pop_back_val();
            if(reached[module].insert(file).second) {
                pending.append(facts.files[file].includes.begin(),
                               facts.files[file].includes.end());
            }
        }
    }
    for(auto& declaration: facts.usings) {
        for(auto file: facts.including_files(declaration.file, true)) {
            if(reached[module_of(file)].contains(file)) {
                exports[module_of(file)].try_emplace(
                    {declaration.name, declaration.file, ExportForm::Name});
            }
        }
    }
    // An unscoped enum's enumerators go by one using-enum-declaration; an
    // enum without a name by the type of its first enumerator by name.
    std::map<std::uint32_t, std::pair<std::string, bool>> unnamed;
    for(std::uint32_t entity = 0; entity < facts.entities.size(); entity += 1) {
        auto& info = facts.entities[entity];
        auto module = module_of(info.owner);
        if(info.kind == SymbolKind::Macro || info.linkage != InternalLinkage::None ||
           info.export_name.empty() || !reached[module].contains(info.owner) ||
           ranges::none_of(info.units.empty() ? facts.files[info.owner].units : info.units,
                           [&](std::uint32_t unit) { return module_of(unit) != module; })) {
            continue;
        }
        auto used = ranges::any_of(reverse.users[entity],
                                   [&](std::uint32_t user) { return module_of(user) != module; });
        auto& enumeration = facts.entities[info.top];
        if(info.kind != SymbolKind::EnumMember || enumeration.kind != SymbolKind::Enum) {
            exports[module][{info.export_name, info.owner, ExportForm::Name}] |= used;
        } else if(!enumeration.export_name.empty()) {
            exports[module][{enumeration.export_name, info.owner, ExportForm::Enum}] |= used;
        } else {
            auto& [first, any_used] = unnamed[info.top];
            if(first.empty() || info.export_name < first) {
                first = info.export_name;
            }
            any_used |= used;
        }
    }
    for(auto& [enumeration, entry]: unnamed) {
        auto& [first, used] = entry;
        auto owner = facts.entities[enumeration].owner;
        exports[module_of(owner)][{first, owner, ExportForm::EnumOf}] |= used;
    }

    // A kept module's header is included by the nearest files up its
    // include chains that files of other modules include, the C library's
    // <stdio.h> for its internal <bits/types/FILE.h>: those are meant to be
    // named. Of several, the one on the chain that brought the header into
    // a unit entering the user.
    auto entry_of = [&](std::uint32_t module, std::uint32_t header, std::uint32_t user) {
        llvm::DenseSet<std::uint32_t> chain;
        for(auto [tree, node]: nodes_of[header]) {
            auto& in = facts.trees[tree];
            if(in.skipped[node] || (in.unit != user && !ranges::contains(in.files, user))) {
                continue;
            }
            // A persisted parent column can be cyclic (see the manifest
            // reader): no chain is longer than the tree.
            for(std::size_t steps = 0; node != none && steps < in.parents.size();
                node = in.parents[node], steps += 1) {
                if(in.files[node] != none) {
                    chain.insert(in.files[node]);
                }
            }
            break;
        }
        llvm::DenseSet<std::uint32_t> visited{header};
        llvm::SmallVector<std::uint32_t> level{header};
        while(!level.empty()) {
            llvm::SmallVector<std::uint32_t> entries, next;
            for(auto file: level) {
                auto& includers = facts.files[file].includers;
                if(ranges::any_of(includers, [&](std::uint32_t includer) {
                       return module_of(includer) != module;
                   })) {
                    entries.push_back(file);
                }
                for(auto includer: includers) {
                    if(module_of(includer) == module && visited.insert(includer).second) {
                        next.push_back(includer);
                    }
                }
            }
            if(!entries.empty()) {
                auto on_chain = ranges::find_if(entries, [&](std::uint32_t file) {
                    return chain.contains(file);
                });
                return on_chain != entries.end() ? *on_chain : entries.front();
            }
            level = std::move(next);
        }
        return header;
    };
    auto header_of = [&](std::uint32_t module, std::uint32_t file, std::string because) {
        auto& info = facts.files[file];
        auto by_path = std::format("\"{}\"", info.path);
        InterfaceHeader header{.file = info.path,
                               .include = by_path,
                               .because = std::move(because)};
        llvm::StringRef own_spelling;
        for(std::size_t i = 0; i < info.includers.size(); i += 1) {
            auto includer = info.includers[i];
            llvm::StringRef spelled = info.spellings[i];
            if(spelled.size() < 2) {
                continue;
            }
            auto name = spelled.drop_front().drop_back();
            if(spelled.starts_with("\"")) {
                std::string beside(path::parent(facts.files[includer].path, path::Style::Posix));
                path::push(beside, name, path::Style::Posix);
                path::normalize(beside, path::DotDot::Fold, path::Style::Posix);
                if(beside == info.path) {
                    continue;
                }
            }
            if(module_of(includer) == module) {
                if(own_spelling.empty()) {
                    own_spelling = spelled;
                }
                continue;
            }
            if(header.include == by_path ||
               (spelled.starts_with("<") && !llvm::StringRef(header.include).starts_with("<"))) {
                header.include = spelled;
            }
            if(!ranges::contains(header.names, name)) {
                header.names.push_back(name.str());
            }
        }
        if(header.include == by_path && !own_spelling.empty()) {
            header.include = own_spelling.str();
        }
        ranges::sort(header.names);
        return header;
    };

    // The TU-local entities each wrapped module's fragment holds that the
    // code an importer can emit uses, from the definitions of its headers
    // through what those call in turn (`_mm_set1_epi8` calls
    // `_mm_set_epi8`), kept headers' inline functions included: an
    // importer's inline code uses the copy of the module whose header it
    // comes from. A constant initialized by a constant expression is no
    // object where it is used by value.
    auto tu_local = [&](const Entity& info) {
        return info.linkage == InternalLinkage::Static ||
               info.linkage == InternalLinkage::AnonymousNamespace;
    };
    auto function = [&](const Entity& info) {
        return ranges::contains(std::array<SymbolKind, 3>{SymbolKind::Function,
                                                          SymbolKind::Method,
                                                          SymbolKind::Operator},
                                info.kind);
    };
    auto emitted = [&](std::uint32_t entity) {
        auto& info = facts.entities[entity];
        return tu_local(info) && !facts.files[info.owner].source &&
               (function(info) ||
                (info.kind == SymbolKind::Variable && (!info.constant || !info.body.empty())));
    };
    std::vector<std::vector<std::uint32_t>> owned(facts.files.size());
    for(std::uint32_t entity = 0; entity < facts.entities.size(); entity += 1) {
        owned[facts.entities[entity].owner].push_back(entity);
    }
    llvm::DenseMap<std::uint32_t, std::set<std::uint32_t>> local_users;
    for(std::uint32_t module = 0; module < count; module += 1) {
        if(partition.kinds[module] != ModuleKind::Wrapped) {
            continue;
        }
        llvm::DenseSet<std::uint32_t> used, passed;
        llvm::SmallVector<std::uint32_t> pending;
        auto use = [&](const BodyUse& named) {
            auto entity = named.entity;
            auto& info = facts.entities[entity];
            if(emitted(entity)) {
                if(used.insert(entity).second) {
                    pending.push_back(entity);
                }
            } else if(function(info) &&
                      partition.kinds[module_of(info.owner)] == ModuleKind::Textual &&
                      passed.insert(entity).second) {
                pending.push_back(entity);
            }
        };
        for(auto file: reached[module]) {
            if(module_of(file) != module) {
                continue;
            }
            for(auto entity: owned[file]) {
                if(!tu_local(facts.entities[entity])) {
                    ranges::for_each(facts.entities[entity].body, use);
                }
            }
        }
        while(!pending.empty()) {
            ranges::for_each(facts.entities[pending.pop_back_val()].body, use);
        }
        for(auto entity: used) {
            local_users[entity].insert(module);
        }
    }

    std::vector<Interface> result;
    for(std::uint32_t module = 0; module < count; module += 1) {
        auto& interface = result.emplace_back();
        interface.module = partition.modules[module];
        // A wrapped module's private headers, which only its sources reach,
        // are no part of its unit.
        for(std::uint32_t file = 0; file < facts.files.size(); file += 1) {
            if(module_of(file) != module ||
               (partition.kinds[module] == ModuleKind::Wrapped && !reached[module].contains(file) &&
                !textual[module].contains(file))) {
                continue;
            }
            imports[module].insert(file_imports[file].begin(), file_imports[file].end());
            reads[module].insert(file_reads[file].begin(), file_reads[file].end());
        }
        imports[module].erase(module);
        for(auto imported: imports[module]) {
            interface.imports.push_back(partition.modules[imported]);
        }
        ranges::sort(interface.imports);
        for(auto file: entries[module]) {
            interface.entries.push_back(header_of(module, file, {}));
        }
        ranges::sort(interface.entries, {}, &InterfaceHeader::file);
        std::map<std::uint32_t, std::string> included;
        for(auto header: textual[module]) {
            if(partition.kinds[module] != ModuleKind::Textual) {
                included.try_emplace(header);
                continue;
            }
            auto [entity, user] = because[module].lookup(header);
            auto reason =
                std::format("{} in {}", facts.entities[entity].name, facts.files[user].path);
            auto entry = entry_of(module, header, user);
            included.try_emplace(entry, reason);
            if(locals[module].contains(header)) {
                interface.locals.push_back(
                    {.file = facts.files[header].path, .entry = header_of(module, entry, reason)});
            }
        }
        for(auto& [file, reason]: included) {
            interface.textual.push_back(header_of(module, file, reason));
        }
        ranges::sort(interface.textual, {}, &InterfaceHeader::file);
        ranges::sort(interface.locals, {}, &InterfaceLocal::file);
        // A kept module's unit also redeclares what the headers its global
        // module fragment includes declare by another name, the aliases and
        // the using-declarations, as no emptied header does.
        llvm::DenseSet<std::uint32_t> fragment;
        if(partition.kinds[module] == ModuleKind::Textual) {
            llvm::SmallVector<std::uint32_t> pending;
            for(auto& [file, reason]: included) {
                pending.push_back(file);
            }
            while(!pending.empty()) {
                auto file = pending.pop_back_val();
                if(module_of(file) == module && fragment.insert(file).second) {
                    pending.append(facts.files[file].includes.begin(),
                                   facts.files[file].includes.end());
                }
            }
            for(auto& declaration: facts.usings) {
                if(ranges::any_of(facts.including_files(declaration.file, true),
                                  [&](std::uint32_t file) { return fragment.contains(file); })) {
                    kept_exports[module].insert(
                        {declaration.name, declaration.file, ExportForm::Name});
                }
            }
        }
        for(auto& [name, file, form]: kept_exports[module]) {
            interface.kept_exports.push_back(
                {.name = name, .file = facts.files[file].path, .used = true, .form = form});
        }
        if(partition.kinds[module] == ModuleKind::Wrapped) {
            // The internal-linkage entities of another wrapped module its
            // headers name: their headers no emptied entry brings in.
            std::set<std::uint32_t> used;
            for(std::uint32_t file = 0; file < facts.files.size(); file += 1) {
                if(module_of(file) != module || facts.files[file].source) {
                    continue;
                }
                for(auto& use: facts.uses[file]) {
                    auto owner = facts.entities[use.entity].owner;
                    auto other = module_of(owner);
                    if(other != module && partition.kinds[other] == ModuleKind::Wrapped &&
                       textual[other].contains(owner)) {
                        used.insert(owner);
                    }
                }
            }
            for(auto header: used) {
                interface.textual_uses.push_back(header_of(module_of(header), header, {}));
            }
            // clang bug: see Interface::shared_locals (clice-io/clice#1057 item 10).
            // Overloads are one name to the reader.
            std::map<std::pair<std::string, std::string>, std::set<std::string>> shared;
            for(auto& [entity, users]: local_users) {
                if(users.size() < 2 || !users.contains(module)) {
                    continue;
                }
                auto& info = facts.entities[entity];
                auto& modules = shared[{info.name, facts.files[info.owner].path}];
                for(auto other: users) {
                    if(other != module) {
                        modules.insert(partition.modules[other]);
                    }
                }
            }
            for(auto& [key, modules]: shared) {
                interface.shared_locals.push_back({
                    .name = key.first,
                    .file = key.second,
                    .modules = {modules.begin(), modules.end()},
                });
            }
        }
        for(auto& [key, used]: exports[module]) {
            auto& [name, file, form] = key;
            interface.exports.push_back(
                {.name = name, .file = facts.files[file].path, .used = used, .form = form});
        }
        auto& unit_files =
            partition.kinds[module] == ModuleKind::Textual ? fragment : reached[module];
        for(auto& alias: facts.aliases) {
            auto charged = facts.charged_files(alias.file);
            if(ranges::any_of(charged, [&](std::uint32_t file) {
                   return module_of(file) == module && unit_files.contains(file);
               })) {
                interface.aliases.push_back({.name = alias.name, .target = alias.target});
            }
        }
        ranges::sort(interface.aliases, {}, &InterfaceAlias::name);

        // What the macros other modules use expand, transitively, within
        // the module: the other modules' macros come with their headers.
        llvm::SmallVector<std::uint32_t> pending(macros[module].begin(), macros[module].end());
        auto& closure = macros[module];
        while(!pending.empty()) {
            for_each_expanded(facts.entities[pending.pop_back_val()].directive,
                              [&](llvm::StringRef word) {
                                  for(auto entity: macros_named[module].lookup(word)) {
                                      if(closure.insert(entity).second) {
                                          pending.push_back(entity);
                                      }
                                  }
                              });
        }
        llvm::SmallVector<std::uint32_t> ordered(closure.begin(), closure.end());
        ranges::sort(ordered, [&](std::uint32_t lhs, std::uint32_t rhs) {
            auto& left = facts.entities[lhs];
            auto& right = facts.entities[rhs];
            return std::tie(facts.files[left.owner].path, left.line) <
                   std::tie(facts.files[right.owner].path, right.line);
        });
        auto macro = [&](std::uint32_t entity) {
            auto& info = facts.entities[entity];
            return InterfaceMacro{
                .name = info.name,
                .module = partition.modules[module_of(info.owner)],
                .file = facts.files[info.owner].path,
                .directive = info.directive,
            };
        };
        for(auto entity: ordered) {
            interface.macros.push_back(macro(entity));
        }
        for(auto entity: reads[module]) {
            interface.reads.push_back(macro(entity));
        }
        ranges::sort(interface.reads, [](auto& lhs, auto& rhs) {
            return std::tie(lhs.file, lhs.name) < std::tie(rhs.file, rhs.name);
        });
    }
    return result;
}

}  // namespace clice::analysis

module clice;

import :analysis.facts.builder;
import :analysis.text;
import :command.file_kind;
import :index.instantiation_shard;
import :index.project_index;
import :index.shard;
import :index.symbol_query;
import :index.tu_manifest;
import :syntax.lexer;
import :syntax.lexical_scan;
import :syntax.token;
import :vfs.file_system;
import :vfs.path;

namespace clice::analysis {

namespace {

/// 1-based line of `offset` in the content a shard describes.
std::uint32_t line_of(const index::Shard& shard, std::uint32_t offset) {
    return lsp::line_of(shard.line_starts(), offset) + 1;
}

/// Whether naming an entity of this kind ties the naming file to the
/// declaring one. Namespaces are reopened everywhere and parameters and
/// labels never leave their function.
bool links_files(SymbolKind kind) {
    switch(kind) {
        case SymbolKind::Class:
        case SymbolKind::Struct:
        case SymbolKind::Union:
        case SymbolKind::Enum:
        case SymbolKind::Type:
        case SymbolKind::Field:
        case SymbolKind::EnumMember:
        case SymbolKind::Function:
        case SymbolKind::Method:
        case SymbolKind::Variable:
        case SymbolKind::Concept:
        case SymbolKind::Operator:
        case SymbolKind::Protocol:
        case SymbolKind::Property:
        case SymbolKind::Macro: return true;
        default: return false;
    }
}

/// Whether `line` of `text` declares `name` a namespace alias:
/// `namespace name =`.
bool declares_alias(const Text& text, std::uint32_t line, llvm::StringRef name) {
    auto tokens = text.tokens_on(line);
    for(std::size_t i = 0; i + 2 < tokens.size(); i += 1) {
        if(tokens[i].text(text.content) == "namespace" &&
           tokens[i + 1].text(text.content) == name && tokens[i + 2].kind == clang::tok::equal) {
            return true;
        }
    }
    return false;
}

}  // namespace

void for_each_identifier(const std::string& code, llvm::function_ref<void(llvm::StringRef)> visit) {
    Lexer lexer(code, raw_dialect(clang::Language::CXX, clang::LangStandard::lang_cxx23));
    for(auto token = lexer.advance(); !token.is_eof(); token = lexer.advance()) {
        if(token.is_identifier()) {
            visit(token.text(code));
        }
    }
}

FactsBuilder::FactsBuilder(Project& project, llvm::function_ref<bool(llvm::StringRef)> in_scope) :
    index(project.index), table(project.file_table), root(project.index.project_root),
    in_scope(in_scope) {}

std::string FactsBuilder::display(Fid fid) const {
    llvm::StringRef identity(table.resolve(fid));
    if(identity.size() > root.size() && path::is_within(identity, root)) {
        if(auto relative = identity.drop_front(root.size()).ltrim('/'); !relative.empty()) {
            return relative.str();
        }
    }
    return std::string(table.display(fid));
}

std::uint32_t FactsBuilder::id_of(Fid fid) const {
    auto it = ids.find(fid);
    return it == ids.end() ? none : it->second;
}

void FactsBuilder::list_files() {
    auto add = [&](Fid fid) {
        auto [it, inserted] = ids.try_emplace(fid, none);
        if(!inserted) {
            return;
        }
        auto path = display(fid);
        if(!in_scope(path)) {
            return;
        }
        it->second = static_cast<std::uint32_t>(facts.files.size());
        facts.file_ids[path] = it->second;
        facts.files.push_back({
            .path = path,
            // A header the database lists has a manifest too.
            .source = index.indexed(fid) && !is_header_path(path),
            .fragment = is_context_header_path(path),
        });
        fids.push_back(fid);
    };
    index.for_each_unit([&](Fid tu) {
        if(auto manifest = index.manifest(tu)) {
            manifests.emplace_back(tu, std::move(*manifest));
        }
    });
    // The database lists files in no meaningful order: the facts take them
    // by path, so they read the same whatever stored them.
    llvm::DenseSet<Fid> listed;
    std::vector<std::pair<std::string, Fid>> paths;
    auto list = [&](Fid fid) {
        if(listed.insert(fid).second) {
            paths.emplace_back(display(fid), fid);
        }
    };
    index.for_each_indexed_file([&](Fid fid, std::uint64_t) { list(fid); });
    for(auto& [tu, manifest]: manifests) {
        list(tu);
        for(auto& node: manifest.nodes) {
            list(table.version(VersionID{node.file}).fid);
        }
    }
    ranges::sort(paths);
    for(auto fid: views::values(paths)) {
        add(fid);
    }
    ranges::sort(manifests, {}, [&](const auto& unit) { return display(unit.first); });

    for(std::uint32_t id = 0; id < facts.files.size(); id += 1) {
        auto& file = facts.files[id];
        if(auto* shard = index.shard(fids[id])) {
            file.lines = static_cast<std::uint32_t>(shard->line_starts().size());
        }
        file.variants = static_cast<std::uint32_t>(index.live_variants(fids[id]).size());
    }
}

void FactsBuilder::read_includes() {
    /// (scoped includer, scoped file) -> the first directive line naming it.
    llvm::DenseMap<std::pair<std::uint32_t, std::uint32_t>, std::uint32_t> include_lines;
    for(auto& [tu, manifest]: manifests) {
        // A header the database lists has a manifest too, though no build
        // compiles it on its own: it names what it includes, and its
        // includers' units are its units.
        auto unit = is_header_path(display(tu)) ? none : id_of(tu);
        llvm::DenseSet<std::uint32_t> entered;
        if(unit != none) {
            entered.insert(unit);
        }
        auto& tree = facts.trees.emplace_back();
        tree.unit = unit;
        for(auto& node: manifest.nodes) {
            auto file = table.version(VersionID{node.file}).fid;
            auto includer = node.parent == index::no_node
                                ? tu
                                : table.version(VersionID{manifest.nodes[node.parent].file}).fid;
            includes[includer].push_back(file);
            include_at[{includer.raw, node.line}] = file;
            tree.files.push_back(id_of(file));
            tree.parents.push_back(node.parent);
            tree.skipped.push_back(node.skipped);
            if(auto from = id_of(includer), to = id_of(file); from != none && to != none) {
                include_lines.try_emplace({from, to}, node.line);
                facts.files[from].directives.emplace_back(node.line, to);
            }
            if(!node.skipped && unit != none) {
                if(auto id = id_of(file); id != none) {
                    entered.insert(id);
                }
            }
        }
        for(auto id: entered) {
            facts.files[id].units.push_back(unit);
        }
        if(unit != none) {
            for(auto& [version, variant]: manifest.contributions) {
                if(auto id = id_of(table.version(version).fid); id != none) {
                    contributors[{id, variant}].push_back(unit);
                }
            }
        }
    }
    for(auto& file: facts.files) {
        ranges::sort(file.directives);
        file.directives.erase(ranges::unique(file.directives).begin(), file.directives.end());
    }
    for(auto& [includer, targets]: includes) {
        ranges::sort(targets);
        targets.erase(ranges::unique(targets).begin(), targets.end());
        auto id = id_of(includer);
        for(auto target: targets) {
            included_by[target].push_back(includer);
            if(auto included = id_of(target); id != none && included != none && included != id) {
                facts.files[id].includes.push_back(included);
                facts.files[included].includers.push_back(id);
            }
        }
    }

    for(std::uint32_t id = 0; id < facts.files.size(); id += 1) {
        auto& file = facts.files[id];
        for(auto includer: file.includers) {
            // A forced include (-include) has no directive in the includer;
            // its presumed line points at some other text there.
            auto& text = text_of(includer);
            auto line = std::max(include_lines.lookup({includer, id}), 1U) - 1;
            llvm::StringRef spelled;
            if(line < text.lines.size() &&
               text.directive_kind(line) == LexicalInfo::Directive::Kind::Include) {
                // A spliced directive's header name starts at its splice.
                spelled = text.operands[line];
                spelled =
                    spelled.drop_front(std::min(spelled.find_first_of("<\""), spelled.size()));
            }
            auto names_file = !spelled.empty() && path::filename(spelled.drop_front().drop_back(),
                                                                 path::Style::Posix) ==
                                                      path::filename(file.path, path::Style::Posix);
            file.spellings.push_back(names_file ? spelled.str() : std::string());
        }
    }
}

const Text& FactsBuilder::text_of(std::uint32_t file) {
    auto& source = sources[file];
    if(!source) {
        source = std::make_unique<Source>();
        if(auto content = vfs::read_text(table.resolve(fids[file]))) {
            source->content = std::move(*content);
        }
        source->text.emplace(source->content);
    }
    return *source->text;
}

llvm::ArrayRef<Token> FactsBuilder::tokens_on(std::uint32_t file, std::uint32_t line) {
    auto& text = text_of(file);
    return line != 0 && line <= text.lines.size() ? text.tokens_on(line - 1)
                                                  : llvm::ArrayRef<Token>();
}

bool FactsBuilder::writes(std::uint32_t file, std::uint32_t line, llvm::StringRef word) {
    auto& text = text_of(file);
    return line != 0 && line <= text.lines.size() && text.names(line - 1, word);
}

std::string FactsBuilder::directive_text(std::uint32_t file, std::uint32_t line) {
    auto& text = text_of(file);
    return line != 0 && line <= text.lines.size() && !text.keywords[line - 1].empty()
               ? text.directive_text(line - 1)
               : std::string();
}

std::optional<index::SymbolView> FactsBuilder::find_symbol(index::SymbolHash hash,
                                                           const index::Shard* shard) const {
    auto identity = index.find_symbol(hash);
    if(!identity && shard) {
        identity = shard->find_symbol(hash);
    }
    return identity;
}

std::string FactsBuilder::qualified_name(index::SymbolHash hash, const index::Shard* shard) const {
    auto identity = find_symbol(hash, shard);
    if(!identity) {
        return std::string();
    }
    auto name = index::SymbolInfo::from(hash, *identity).display_name();
    index::for_each_spelled_scope(
        hash,
        identity->parent,
        [&](index::SymbolHash scope) { return find_symbol(scope, shard); },
        [&](index::SymbolHash scope, const index::SymbolView& view) {
            name = index::SymbolInfo::from(scope, view).display_name() + "::" + name;
            return true;
        });
    return name;
}

std::string FactsBuilder::export_name_of(index::SymbolHash hash,
                                         const index::SymbolView& identity,
                                         const index::Shard* shard) const {
    // A deduction guide has no name to export it by.
    if(identity.kind == SymbolKind::Macro ||
       index::has_flag(identity.flags, index::SymbolFlags::Specialization) ||
       index::name_form(identity.flags) == index::NameForm::Other) {
        return {};
    }
    auto parent = identity.parent != 0 ? find_symbol(identity.parent, shard) : std::nullopt;
    if(!parent || parent->kind == SymbolKind::Namespace) {
        return index::has_flag(identity.flags, index::SymbolFlags::Unnamed)
                   ? std::string()
                   : qualified_name(hash, shard);
    }
    if(identity.kind != SymbolKind::EnumMember || parent->kind != SymbolKind::Enum ||
       !index::has_flag(identity.flags, index::SymbolFlags::Completable)) {
        return {};
    }
    auto grand = parent->parent != 0 ? find_symbol(parent->parent, shard) : std::nullopt;
    if(grand && grand->kind != SymbolKind::Namespace) {
        return {};
    }
    auto enclosing = qualified_name(identity.parent, shard);
    auto separator = llvm::StringRef(enclosing).rfind("::");
    return (separator == llvm::StringRef::npos ? std::string()
                                               : enclosing.substr(0, separator + 2)) +
           identity.name.str();
}

void FactsBuilder::read_shards() {
    raw.resize(facts.files.size());
    pasted_lines.resize(facts.files.size());
    use_sites.resize(facts.files.size());
    weak_sites.resize(facts.files.size());
    for(std::uint32_t id = 0; id < facts.files.size(); id += 1) {
        auto* found = index.shard(fids[id]);
        if(!found) {
            continue;
        }
        auto& shard = *found;
        auto names = [](const index::Shard& from,
                        llvm::function_ref<void(index::SymbolHash, const index::Relation&)> visit) {
            from.for_each_relation([&](index::SymbolHash hash, const index::Relation& relation) {
                visit(hash, relation);
                return true;
            });
        };
        auto use_of = [](const index::Relation& relation) {
            return RelationKind(relation.kind)
                .is_one_of(RelationKind::Reference,
                           RelationKind::Candidate,
                           RelationKind::Guess,
                           RelationKind::Read,
                           RelationKind::Write,
                           RelationKind::Pasted);
        };
        auto declaration_of = [](const index::Relation& relation) {
            return RelationKind(relation.kind).declares();
        };

        // A file the index holds several variants of carries, in only some
        // of them, what the including context decides. Those uses still
        // count, as the dependence of some configuration.
        auto live = index.live_variants(fids[id]);
        if(live.size() > 1) {
            llvm::DenseMap<index::SymbolHash, std::uint32_t> carried, declaring;
            std::optional<llvm::DenseSet<index::SymbolHash>> declared;
            auto variant_view = index::Shard::from_bytes(shard.bytes());
            for(auto variant: live) {
                variant_view.set_live({variant});
                llvm::DenseSet<index::SymbolHash> named, declares;
                names(variant_view, [&](index::SymbolHash hash, const index::Relation& relation) {
                    if(use_of(relation)) {
                        named.insert(hash);
                    } else if(declaration_of(relation)) {
                        declares.insert(hash);
                    }
                });
                for(auto hash: named) {
                    carried[hash] += 1;
                }
                if(declared && *declared != declares) {
                    facts.files[id].declarations_differ = true;
                }
                for(auto hash: declares) {
                    declaring[hash] += 1;
                    auto& units = declaring_units[{id, hash}];
                    units.append(contributors.lookup({id, variant}));
                }
                declared = std::move(declares);
            }
            for(auto& [hash, count]: declaring) {
                if(count != live.size()) {
                    partially_declared.insert({id, hash});
                }
            }
            auto& unstable = facts.files[id].unstable;
            for(auto& [hash, count]: carried) {
                if(count != live.size()) {
                    unstable.push_back(qualified_name(hash, &shard));
                }
            }
            ranges::sort(unstable);
            unstable.erase(ranges::unique(unstable).begin(), unstable.end());
        }

        names(shard, [&](index::SymbolHash hash, const index::Relation& relation) {
            if(declaration_of(relation)) {
                sites[hash].push_back({
                    .file = id,
                    .line = line_of(shard, relation.range.begin),
                    .offset = relation.range.begin,
                    .definition = relation.kind == RelationKind::Definition,
                    .extent = relation.extent(),
                });
            } else if(relation.kind == RelationKind::Primary) {
                raw_specializations.emplace_back(hash, relation.target, id);
            } else if(use_of(relation)) {
                auto& use = raw[id][hash];
                if(use.count == 0) {
                    use.line = line_of(shard, relation.range.begin);
                }
                if(relation.kind == RelationKind::Pasted) {
                    pasted_lines[id].insert(line_of(shard, relation.range.begin));
                }
                use.count += 1;
                bool weak = RelationKind(relation.kind)
                                .is_one_of(RelationKind::Candidate, RelationKind::Guess);
                use.strong |= !weak;
                use_sites[id].emplace_back(relation.range.begin, hash);
                if(weak) {
                    weak_sites[id].emplace_back(relation.range.begin, hash);
                }
            }
        });
    }

    definitions.resize(facts.files.size());
    for(auto& [hash, list]: sites) {
        for(auto& site: list) {
            if(site.extent.end > site.extent.begin) {
                definitions[site.file].emplace_back(site.extent, hash);
            }
        }
    }
}

void FactsBuilder::read_instantiations() {
    // A dependent call by a qualified name (`ops::clamp(value)`) looks its
    // overloads up where the template is defined, as a non-dependent name
    // does: the one an instantiation runs is the file's to see, even when
    // only the including unit declared it. What argument dependent lookup,
    // a dependent type's member or a specialization for the arguments
    // brings is the instantiating unit's, which a module defining the
    // template never imports. An instantiation running a candidate the
    // file's own entries name records nothing.
    for(std::uint32_t id = 0; id < facts.files.size(); id += 1) {
        auto* instantiations = index.instantiation_shard(fids[id]);
        auto* shard = index.shard(fids[id]);
        if(!instantiations || !shard) {
            continue;
        }
        instantiations->for_each_record([&](const index::InstantiationRecord& record) {
            if(record.kind != index::InstantiationKind::Reference || record.expansion) {
                return true;
            }
            auto identity = find_symbol(record.target, shard);
            auto parent = identity && identity->parent != 0 ? find_symbol(identity->parent, shard)
                                                            : std::nullopt;
            if(!identity || index::has_flag(identity->flags, index::SymbolFlags::Specialization) ||
               (parent && parent->kind != SymbolKind::Namespace)) {
                return true;
            }
            auto& tokens = text_of(id).tokens;
            auto name = ranges::lower_bound(tokens, record.range.begin, {}, [](const Token& token) {
                return token.range.begin;
            });
            if(name == tokens.begin() || name == tokens.end() ||
               name->range.begin != record.range.begin ||
               std::prev(name)->kind != clang::tok::coloncolon) {
                return true;
            }
            auto& use = raw[id][record.target];
            if(use.count == 0) {
                use.line = line_of(*shard, record.range.begin);
            }
            use.count += 1;
            use.strong = true;
            use_sites[id].emplace_back(record.range.begin, record.target);
            return true;
        });
    }
}

void FactsBuilder::find_pasters() {
    // A fragment pasted inside a declaration: the includer's Pasted relations at
    // the directive name what the fragment names there.
    for(std::uint32_t id = 0; id < facts.files.size(); id += 1) {
        for(auto line: pasted_lines[id]) {
            auto it = include_at.find({fids[id].raw, line});
            if(it == include_at.end()) {
                continue;
            }
            if(auto fragment = id_of(it->second);
               fragment != none && facts.files[fragment].fragment &&
               !ranges::contains(facts.files[fragment].pasters, id)) {
                facts.files[fragment].pasters.push_back(id);
            }
        }
    }
}

void FactsBuilder::read_macros() {
    // A macro definition some #undef removes is a helper scoped to the lines
    // between (an X-macro around a table): the #undef's relation references it.
    // A header included again undefines its own macro ahead of defining it
    // anew (<assert.h>, <limits.h>): that #undef ends nothing.
    for(std::uint32_t id = 0; id < facts.files.size(); id += 1) {
        for(auto& [offset, hash]: use_sites[id]) {
            if(undefined.contains(hash)) {
                continue;
            }
            auto* shard = index.shard(fids[id]);
            auto identity = find_symbol(hash, shard);
            if(!identity || identity->kind != SymbolKind::Macro) {
                continue;
            }
            auto line = line_of(*shard, offset);
            auto& text = text_of(id);
            auto start = line <= text.lines.size() ? text.directive_line(line - 1) : std::nullopt;
            if(!start || text.keywords[*start] != "undef") {
                continue;
            }
            // Elsewhere, an #undef is one file's cleanup (a consumer dropping
            // <windows.h>'s min), not the end of the macro.
            auto here = sites.lookup(hash);
            auto scoped = facts.files[id].fragment || ranges::any_of(here, [&](const Site& site) {
                              return site.file == id && site.line < line;
                          });
            auto redefined = ranges::any_of(here, [&](const Site& site) {
                return site.file == id && site.line > line;
            });
            if(scoped && !redefined) {
                undefined.insert(hash);
            }
        }
    }

    for(auto& [hash, list]: sites) {
        if(auto identity = find_symbol(hash, index.shard(fids[list.front().file]));
           identity && identity->kind == SymbolKind::Macro) {
            for(auto& site: list) {
                macro_definitions[identity->name].emplace_back(site.file, site.line);
            }
        }
    }
}

bool FactsBuilder::friend_site(const Site& site, llvm::StringRef name) {
    auto first = site.line;
    if(site.extent.end > site.extent.begin) {
        first = std::min(first, line_of(*index.shard(fids[site.file]), site.extent.begin));
    } else if(auto tokens = tokens_on(site.file, first);
              first > 1 && !tokens.empty() &&
              text_of(site.file).content.substr(tokens.front().range.begin).starts_with(name)) {
        first -= 1;
    }
    bool found = false;
    for(auto line = first; line <= site.line && !found; line += 1) {
        found = writes(site.file, line, "friend");
        for(const auto& token: tokens_on(site.file, line)) {
            if(!token.is_identifier()) {
                continue;
            }
            for(auto [file, at]: macro_definitions.lookup(token.text(text_of(site.file).content))) {
                for_each_identifier(directive_text(file, at), [&](llvm::StringRef word) {
                    found = found || word == "friend";
                });
            }
        }
    }
    return found;
}

std::optional<std::string> FactsBuilder::namespace_at(std::uint32_t file,
                                                      std::uint32_t offset,
                                                      index::SymbolHash self) const {
    const std::pair<LocalSourceRange, index::SymbolHash>* innermost = nullptr;
    for(auto& definition: definitions[file]) {
        if(definition.second != self && definition.first.begin <= offset &&
           offset < definition.first.end &&
           (!innermost || innermost->first.begin <= definition.first.begin)) {
            innermost = &definition;
        }
    }
    if(!innermost) {
        return std::string();
    }
    auto* shard = index.shard(fids[file]);
    auto enclosing = find_symbol(innermost->second, shard);
    if(!enclosing || enclosing->kind != SymbolKind::Namespace) {
        return std::nullopt;
    }
    return qualified_name(innermost->second, shard);
}

void FactsBuilder::find_usings() {
    // A using-declaration at namespace scope (`namespace hlsl { using
    // dxil::ResourceClass; }`) re-declares a name there: a candidate of the
    // target on a `using` line in a namespace.
    for(std::uint32_t id = 0; id < facts.files.size(); id += 1) {
        auto* shard = index.shard(fids[id]);
        for(auto& [offset, target]: weak_sites[id]) {
            auto identity = find_symbol(target, shard);
            if(!identity || identity->name.empty()) {
                continue;
            }
            auto line = line_of(*shard, offset);
            auto tokens = tokens_on(id, line);
            bool qualified = false;
            for(std::size_t i = 1; i < tokens.size(); i += 1) {
                qualified |= tokens[i - 1].kind == clang::tok::coloncolon &&
                             tokens[i].text(text_of(id).content) == identity->name;
            }
            if(!writes(id, line, "using") || writes(id, line, "namespace") || !qualified) {
                continue;
            }
            auto scope = namespace_at(id, offset, 0);
            if(!scope) {
                continue;
            }
            facts.usings.push_back({
                .name = (scope->empty() ? "" : *scope + "::") + identity->name.str(),
                .file = id,
            });
        }
    }
}

void FactsBuilder::find_aliases() {
    // A namespace alias at namespace scope: its definition relation, and on its
    // line the last relation naming a namespace, the one it aliases.
    for(auto& [hash, list]: sites) {
        auto identity = index.find_symbol(hash);
        if(!identity || identity->kind != SymbolKind::Namespace) {
            continue;
        }
        auto parent = identity->parent != 0 ? index.find_symbol(identity->parent) : std::nullopt;
        if(parent && parent->kind != SymbolKind::Namespace) {
            continue;
        }
        auto site = ranges::find_if(list, [&](const Site& site) {
            return !facts.files[site.file].source && site.line != 0 &&
                   site.line <= text_of(site.file).lines.size() &&
                   declares_alias(text_of(site.file), site.line - 1, identity->name);
        });
        if(site == list.end()) {
            continue;
        }
        auto* shard = index.shard(fids[site->file]);
        index::SymbolHash target = 0;
        std::uint32_t last = 0;
        for(auto& [offset, used]: use_sites[site->file]) {
            if(used == hash || offset < last || line_of(*shard, offset) != site->line) {
                continue;
            }
            if(auto named = find_symbol(used, shard);
               named && named->kind == SymbolKind::Namespace) {
                target = used;
                last = offset;
            }
        }
        if(target != 0) {
            facts.aliases.push_back({
                .name = qualified_name(hash, shard),
                .target = "::" + qualified_name(target, shard),
                .file = site->file,
            });
        }
    }
}

void FactsBuilder::read_entities() {
    for(auto& entry: sites) {
        classify(entry.first);
    }
    for(auto& uses: raw) {
        for(auto& entry: uses) {
            classify(entry.first);
        }
    }
    for(auto& [specialization, primary, file]: raw_specializations) {
        classify(primary);
    }
    link_members();
}

std::uint32_t FactsBuilder::entity_of(index::SymbolHash hash) const {
    auto it = entity_ids.find(hash);
    return it == entity_ids.end() ? none : it->second;
}

void FactsBuilder::classify(index::SymbolHash hash) {
    auto [it, inserted] = entity_ids.try_emplace(hash, none);
    if(!inserted) {
        return;
    }
    auto site_it = sites.find(hash);
    llvm::ArrayRef<Site> scoped;
    if(site_it != sites.end()) {
        scoped = site_it->second;
    }

    std::optional<index::SymbolView> identity = index.find_symbol(hash);
    bool internal = !identity;
    for(std::size_t i = 0; !identity && i < scoped.size(); i += 1) {
        if(auto* shard = index.shard(fids[scoped[i].file])) {
            identity = shard->find_symbol(hash);
        }
    }
    if(!identity || identity->scope == index::SymbolScope::FunctionLocal ||
       !links_files(identity->kind)) {
        return;
    }
    auto kind = identity->kind;
    Fid canonical;
    if(!internal && identity->file != index::no_file) {
        canonical = Fid{identity->file};
    }
    auto owner = owner_of(hash, *identity, scoped, canonical);
    if(owner == none) {
        return;
    }
    auto declared_in = owner;
    if(facts.files[owner].fragment) {
        owner = paster_of(owner, hash, *identity, scoped);
        if(owner == none) {
            return;
        }
    }
    auto* owner_shard = index.shard(fids[declared_in]);
    auto site = ranges::find(scoped, declared_in, &Site::file);
    auto line = site != scoped.end() ? site->line : 0;

    auto linkage = InternalLinkage::None;
    if(internal) {
        linkage = kind == SymbolKind::Variable ? InternalLinkage::Const : InternalLinkage::Static;
        // A parent skips an anonymous namespace, as a qualified name
        // does; its definition still encloses the declaration.
        if(site != scoped.end() &&
           ranges::any_of(definitions[declared_in], [&](const auto& definition) {
               auto scope = find_symbol(definition.second, owner_shard);
               return definition.first.begin <= site->offset &&
                      site->offset < definition.first.end && scope &&
                      scope->kind == SymbolKind::Namespace &&
                      index::has_flag(scope->flags, index::SymbolFlags::Unnamed);
           })) {
            linkage = InternalLinkage::AnonymousNamespace;
        } else if(linkage == InternalLinkage::Const && writes(declared_in, line, "static")) {
            linkage = InternalLinkage::Static;
        }
    }
    bool constant = kind == SymbolKind::Variable &&
                    (writes(declared_in, line, "const") || writes(declared_in, line, "constexpr"));

    // The index names a C-linkage function without its namespace, one
    // entity across all of them; C++ code names it by the namespace
    // declaring it (CRoaring's roaring::internal under __cplusplus).
    auto c_linkage_name = [&](std::string name) {
        if(name.empty() || llvm::StringRef(name).contains("::") || site == scoped.end()) {
            return name;
        }
        auto scope = namespace_at(declared_in, site->offset, hash);
        return scope && !scope->empty() ? *scope + "::" + name : name;
    };

    auto id = static_cast<std::uint32_t>(facts.entities.size());
    it->second = id;
    facts.entities.push_back({
        .hash = hash,
        .name =
            kind == SymbolKind::Macro ? identity->name.str() : qualified_name(hash, owner_shard),
        .kind = kind,
        .owner = owner,
        .declared_in = declared_in,
        .linkage = linkage,
        .top = id,
        .line = line,
        .self_uses = raw[owner].lookup(hash).count +
                     (declared_in != owner ? raw[declared_in].lookup(hash).count : 0),
        .self_line = raw[owner].lookup(hash).line,
        .constant = constant,
        .units =
            [&] {
                auto units = declaring_units.lookup({declared_in, hash});
                ranges::sort(units);
                units.erase(ranges::unique(units).begin(), units.end());
                return std::vector<std::uint32_t>(units.begin(), units.end());
            }(),
        .partial = partially_declared.contains({declared_in, hash}),
        // A hidden friend has no name lookup finds; argument dependent
        // lookup reaches it through its class.
        .export_name = ranges::contains(std::array<SymbolKind, 5>{SymbolKind::Function,
                                                                  SymbolKind::Operator,
                                                                  SymbolKind::Class,
                                                                  SymbolKind::Struct,
                                                                  SymbolKind::Union},
                                        kind) &&
                               ranges::all_of(scoped,
                                              [&](const Site& site) {
                                                  return friend_site(site, identity->name);
                                              })
                           ? std::string()
                           : c_linkage_name(export_name_of(hash, *identity, owner_shard)),
        .directive = kind == SymbolKind::Macro ? directive_text(declared_in, line) : std::string(),
        .undefined = undefined.contains(hash),
    });
    parents.push_back(identity->parent);
    operators.push_back(index::name_form(identity->flags) == index::NameForm::Operator);
    if(kind == SymbolKind::Macro) {
        // Out-of-scope headers reading a scoped macro are configured by
        // it; out-of-scope sources merely use it, and fragments are
        // pasted where the macro is defined.
        if(!facts.files[owner].source) {
            ConfiguringMacro configuring{.entity = id};
            index.for_each_reference_file(hash, [&](Fid file) {
                if(id_of(file) != none || index.indexed(file)) {
                    return;
                }
                auto path = display(file);
                if(!is_context_header_path(path)) {
                    configuring.readers.push_back(std::move(path));
                }
            });
            if(!configuring.readers.empty()) {
                facts.configuring_macros.push_back(std::move(configuring));
            }
        }
        return;
    }

    llvm::SmallVector<std::uint32_t> defining_headers;
    for(auto& site: scoped) {
        if(site.definition && !facts.files[site.file].source &&
           !ranges::contains(defining_headers, site.file)) {
            defining_headers.push_back(site.file);
        }
        if(site.file != owner && site.file != declared_in) {
            facts.redeclarations.push_back({
                .entity = id,
                .file = site.file,
                .line = site.line,
                .definition = site.definition,
                .friend_declaration = writes(site.file, site.line, "friend"),
            });
        }
    }
    if(defining_headers.size() > 1) {
        facts.duplicate_definitions.push_back({
            .entity = id,
            .files = {defining_headers.begin(), defining_headers.end()},
        });
    }
}

void FactsBuilder::read_specializations() {
    // The specialization's file needs the template it specializes. One of
    // an out-of-scope template in a header is used wherever that template
    // is instantiated, unseen, unless its arguments name what the header
    // provides: those users name it too.
    std::vector<llvm::SmallVector<std::uint32_t>> owned(facts.files.size());
    for(std::uint32_t entity = 0; entity < facts.entities.size(); entity += 1) {
        if(facts.entities[entity].kind != SymbolKind::Macro) {
            owned[facts.entities[entity].owner].push_back(entity);
        }
    }
    auto names_owned = [&](index::SymbolHash specialization, std::uint32_t file) {
        auto it = sites.find(specialization);
        return it != sites.end() && ranges::any_of(it->second, [&](const Site& site) {
                   if(site.file != file) {
                       return false;
                   }
                   // From the name to the body: the arguments and the bases.
                   auto& text = text_of(file);
                   auto from =
                       ranges::lower_bound(text.tokens, site.offset, {}, [](const Token& token) {
                           return token.range.begin;
                       });
                   auto head = ranges::subrange(
                       from,
                       std::find_if(from, text.tokens.end(), [](const Token& token) {
                           return token.kind == clang::tok::l_brace ||
                                  token.kind == clang::tok::semi;
                       }));
                   return ranges::any_of(owned[file], [&](std::uint32_t entity) {
                       // The specialization and its members are no arguments.
                       if(facts.entities[entity].hash == specialization ||
                          parents[entity] == specialization) {
                           return false;
                       }
                       llvm::StringRef name = facts.entities[entity].name;
                       auto separator = name.rfind("::");
                       auto last =
                           separator == llvm::StringRef::npos ? name : name.substr(separator + 2);
                       return ranges::any_of(head, [&](const Token& token) {
                           return token.is_identifier() && token.text(text.content) == last;
                       });
                   });
               });
    };
    for(auto& [specialization, primary, file]: raw_specializations) {
        auto entity = entity_of(primary);
        if(entity == none) {
            if(facts.files[file].source || names_owned(specialization, file)) {
                continue;
            }
            if(auto name = qualified_name(primary, index.shard(fids[file]));
               !name.empty() && !ranges::contains(facts.files[file].specializes, name)) {
                facts.files[file].specializes.push_back(std::move(name));
            }
            continue;
        }
        if(facts.entities[entity].owner == file) {
            continue;
        }
        if(ranges::none_of(facts.specializations, [&](const Specialization& known) {
               return known.primary == entity && known.file == file;
           })) {
            std::uint32_t line = 0;
            if(auto it = sites.find(specialization); it != sites.end()) {
                if(auto site = ranges::find_if(it->second,
                                               [&](const Site& site) { return site.file == file; });
                   site != it->second.end()) {
                    line = site->line;
                }
            }
            facts.specializations.push_back({.primary = entity, .file = file, .line = line});
        }
        auto& use = raw[file][primary];
        use.count += 1;
        use.strong = true;
    }
}

llvm::DenseSet<Fid> FactsBuilder::include_closure(std::uint32_t file) const {
    llvm::DenseSet<Fid> closure;
    llvm::SmallVector<Fid> pending{fids[file]};
    while(!pending.empty()) {
        auto current = pending.pop_back_val();
        if(!closure.insert(current).second) {
            continue;
        }
        if(auto it = includes.find(current); it != includes.end()) {
            pending.append(it->second.begin(), it->second.end());
        }
    }
    return closure;
}

void FactsBuilder::read_uses() {
    // A file defining an entity itself, as a duplicate definition does,
    // needs no other file for it.
    auto defines = [&](std::uint32_t file, index::SymbolHash hash) {
        auto it = sites.find(hash);
        return it != sites.end() && ranges::any_of(it->second, [&](const Site& site) {
                   return site.file == file && site.definition;
               });
    };
    facts.uses.resize(facts.files.size());
    facts.macro_uses.resize(facts.files.size());
    for(std::uint32_t id = 0; id < facts.files.size(); id += 1) {
        // A dependent call's candidate declared outside the file's include
        // closure came from what the including unit included first, as
        // llvm's std::swap overloads reach a template calling std::swap.
        std::optional<llvm::DenseSet<Fid>> closure;
        auto visible = [&](index::SymbolHash hash) {
            if(facts.files[id].fragment) {
                return true;
            }
            if(!closure) {
                closure = include_closure(id);
            }
            auto it = sites.find(hash);
            return it != sites.end() && ranges::any_of(it->second, [&](const Site& site) {
                       return closure->contains(fids[site.file]);
                   });
        };
        for(auto& [hash, use]: raw[id]) {
            auto entity = entity_of(hash);
            if(entity == none || facts.entities[entity].owner == id || defines(id, hash) ||
               (!use.strong && (operators[entity] || !visible(hash)))) {
                continue;
            }
            auto& into = facts.entities[entity].kind == SymbolKind::Macro ? facts.macro_uses[id]
                                                                          : facts.uses[id];
            into.push_back({.entity = entity, .count = use.count, .line = use.line});
        }
        for(auto* list: {&facts.uses[id], &facts.macro_uses[id]}) {
            ranges::sort(*list, {}, &Use::entity);
        }
        ranges::sort(facts.files[id].specializes);
    }

    // Each use relation inside a definition the file provides, or a fragment
    // it pastes in holds, belongs to the innermost such definition: what
    // moves with it.
    struct Extent {
        std::uint32_t begin = 0;
        std::uint32_t end = 0;
        std::uint32_t entity = 0;
    };

    std::vector<llvm::SmallVector<Extent>> extents(facts.files.size());
    for(auto& [hash, list]: sites) {
        auto entity = entity_of(hash);
        if(entity == none) {
            continue;
        }
        auto owner = facts.entities[entity].owner;
        for(auto& site: list) {
            if(site.definition && site.extent.end > site.extent.begin &&
               (site.file == owner ||
                (facts.files[site.file].fragment &&
                 ranges::contains(facts.including_files(site.file, true), owner)))) {
                extents[site.file].push_back({site.extent.begin, site.extent.end, entity});
            }
        }
    }
    for(std::uint32_t id = 0; id < facts.files.size(); id += 1) {
        auto& definitions = extents[id];
        if(definitions.empty()) {
            continue;
        }
        ranges::sort(definitions, [](const Extent& lhs, const Extent& rhs) {
            return std::tuple(lhs.begin, rhs.end) < std::tuple(rhs.begin, lhs.end);
        });
        auto& file_sites = use_sites[id];
        ranges::sort(file_sites);
        std::vector<std::pair<std::uint32_t, std::uint32_t>> owned_uses;
        llvm::SmallVector<const Extent*> open;
        std::size_t next = 0;
        for(auto& [offset, hash]: file_sites) {
            for(; next < definitions.size() && definitions[next].begin <= offset; next += 1) {
                while(!open.empty() && open.back()->end <= definitions[next].begin) {
                    open.pop_back();
                }
                open.push_back(&definitions[next]);
            }
            while(!open.empty() && open.back()->end <= offset) {
                open.pop_back();
            }
            auto used = entity_of(hash);
            if(open.empty() || used == none || facts.entities[used].kind == SymbolKind::Macro) {
                continue;
            }
            // Only relations that are uses or self-uses count.
            auto& use = raw[id][hash];
            if(facts.entities[used].owner == id ||
               (!defines(id, hash) && (use.strong || !operators[used]))) {
                owned_uses.emplace_back(open.back()->entity, used);
            }
        }
        ranges::sort(owned_uses);
        for(auto& [entity, used]: owned_uses) {
            auto& body = facts.entities[entity].body;
            if(body.empty() || body.back().file != id || body.back().entity != used) {
                body.push_back({.file = id, .entity = used});
            }
            body.back().count += 1;
        }
    }
}

void FactsBuilder::find_context_macros() {
    // A header naming a macro it does not include the definition of reads
    // differently compiled alone. Fragments stay textual by design and read
    // their includer's macros.
    for(std::uint32_t id = 0; id < facts.files.size(); id += 1) {
        if(facts.files[id].source || facts.files[id].fragment) {
            continue;
        }
        auto closure = include_closure(id);
        // Expanded anywhere, the macro breaks the header compiled alone;
        // only tested, it silently changes how the header reads.
        auto* shard = index.shard(fids[id]);
        auto report = [&](index::SymbolHash hash, llvm::StringRef name, Fid definition) {
            if(closure.contains(definition)) {
                return;
            }
            auto tested = none;
            auto expanded = none;
            for(auto& [offset, used]: use_sites[id]) {
                if(used != hash) {
                    continue;
                }
                auto line = line_of(*shard, offset);
                auto& text = text_of(id);
                if(auto start =
                       line <= text.lines.size() ? text.directive_line(line - 1) : std::nullopt) {
                    using enum LexicalInfo::Directive::Kind;
                    auto directive = text.keywords[*start];
                    // Naming it there needs no definition.
                    if(directive == "define" || directive == "undef") {
                        continue;
                    }
                    if(auto kind = text.directive_kind(*start); kind == If || kind == Else) {
                        tested = std::min(tested, line);
                        continue;
                    }
                }
                expanded = std::min(expanded, line);
            }
            if(tested == none && expanded == none) {
                return;
            }
            facts.context_macros.push_back({
                .name = name.str(),
                .definition = display(definition),
                .file = id,
                .line = expanded != none ? expanded : tested,
                .in_condition = expanded == none,
            });
        };
        for(auto& use: facts.macro_uses[id]) {
            auto& macro = facts.entities[use.entity];
            report(macro.hash, macro.name, fids[macro.owner]);
        }
        for(auto& entry: raw[id]) {
            if(auto it = foreign_macros.find(entry.first); it != foreign_macros.end()) {
                report(entry.first, it->second.second, it->second.first);
            }
        }
    }
}

}  // namespace clice::analysis

module clice;

import :analysis.module_graph;
import :analysis.text;
import :command.file_kind;
import :config.config;
import :index.project_index;
import :index.shard;
import :index.symbol_query;
import :index.tu_manifest;
import :project.configuration;
import :project.index_reader;
import :project.project;
import :syntax.lexer;
import :syntax.lexical_scan;
import :syntax.token;
import :vfs.file_system;
import :vfs.path;

namespace clice::analysis {

namespace {

constexpr std::uint32_t none = ~0u;

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
        case SymbolKind::Macro: return true;
        default: return false;
    }
}

struct Site {
    std::uint32_t file = 0;
    std::uint32_t line = 0;

    /// Where the declared name starts.
    std::uint32_t offset = 0;
    bool definition = false;

    /// The declaration's extent, empty when the index holds none.
    LocalSourceRange extent{0, 0};
};

struct RawUse {
    std::uint32_t count = 0;
    std::uint32_t line = 0;

    /// Some relation names it outright rather than as an overload candidate.
    bool strong = false;
};

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

/// Visit each identifier of `code`.
void for_each_identifier(const std::string& code, llvm::function_ref<void(llvm::StringRef)> visit) {
    Lexer lexer(code, raw_dialect(clang::Language::CXX, clang::LangStandard::lang_cxx23));
    for(auto token = lexer.advance(); !token.is_eof(); token = lexer.advance()) {
        if(token.is_identifier()) {
            visit(token.text(code));
        }
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

/// The files a file stands for: itself, or for a fragment the files
/// including it, through fragments including fragments; with `pasted`
/// false, none for a fragment some file pastes inside a declaration.
llvm::SmallVector<std::uint32_t> including_files(const Facts& facts,
                                                 std::uint32_t file,
                                                 bool pasted) {
    llvm::SmallVector<std::uint32_t> result;
    llvm::SmallVector<std::uint32_t> pending{file};
    llvm::DenseSet<std::uint32_t> visited;
    while(!pending.empty()) {
        auto current = pending.pop_back_val();
        if(!visited.insert(current).second) {
            continue;
        }
        if(!facts.files[current].fragment) {
            result.push_back(current);
            continue;
        }
        if(pasted || facts.files[current].pasters.empty()) {
            pending.append(facts.files[current].includers.begin(),
                           facts.files[current].includers.end());
        }
    }
    return result;
}

/// The files a file's uses are charged to: itself, or for a fragment the
/// files including it. A table pasted inside declarations is charged to
/// none: each file pasting it names what it names there through its own
/// Pasted relations, and its own shard merges what every pasting context made of
/// it (clang's Options.inc expands to enumerators in clang and to option
/// tables naming the program's types in the program).
llvm::SmallVector<std::uint32_t> charged_files(const Facts& facts, std::uint32_t file) {
    return including_files(facts, file, false);
}

}  // namespace

Facts collect(Project& project, llvm::function_ref<bool(llvm::StringRef)> in_scope) {
    Facts facts;
    auto& index = project.index;
    auto& table = project.file_table;
    llvm::StringRef root(index.workspace);

    // A path relative to the workspace, empty for one outside it.
    auto relative_of = [&](Fid fid) {
        llvm::StringRef identity(table.resolve(fid));
        if(identity.size() <= root.size() || !path::is_within(identity, root)) {
            return std::string();
        }
        return identity.drop_front(root.size()).ltrim('/').str();
    };
    auto display = [&](Fid fid) {
        auto relative = relative_of(fid);
        return relative.empty() ? std::string(table.display(fid)) : relative;
    };

    // The scoped files are the indexed ones: those holding a shard, the
    // translation units, and the files their include trees enter.
    llvm::DenseMap<Fid, std::uint32_t> ids;
    std::vector<Fid> fids;
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
            // A header an editor indexed standalone has a manifest too.
            .source = index.indexed(fid) && !is_header_path(path),
            .fragment = is_context_header_path(path),
        });
        fids.push_back(fid);
    };
    std::vector<std::pair<Fid, index::TUManifest>> manifests;
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
    index.for_each_indexed_file(list);
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
    auto id_of = [&](Fid fid) {
        auto it = ids.find(fid);
        return it == ids.end() ? none : it->second;
    };

    for(std::uint32_t id = 0; id < facts.files.size(); id += 1) {
        auto& file = facts.files[id];
        if(auto* shard = index.shard(fids[id])) {
            file.lines = static_cast<std::uint32_t>(shard->line_starts().size());
        }
        file.variants = static_cast<std::uint32_t>(index.live_variants(fids[id]).size());
    }

    // Include edges of every file, scoped or not: a header's include
    // closure runs through third-party headers.
    llvm::DenseMap<Fid, llvm::SmallVector<Fid>> includes;
    llvm::DenseMap<Fid, llvm::SmallVector<Fid, 2>> included_by;
    /// (file, variant hash) -> the scoped units whose index of the file is
    /// that variant.
    llvm::DenseMap<std::pair<std::uint32_t, std::uint64_t>, llvm::SmallVector<std::uint32_t, 2>>
        contributors;
    /// (includer, directive line) -> the file the directive includes.
    llvm::DenseMap<std::pair<std::uint32_t, std::uint32_t>, Fid> include_at;
    /// (scoped includer, scoped file) -> the first directive line naming it.
    llvm::DenseMap<std::pair<std::uint32_t, std::uint32_t>, std::uint32_t> include_lines;
    for(auto& [tu, manifest]: manifests) {
        auto unit = id_of(tu);
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

    // A scoped file's text, from the disk (the index holds positions, not
    // the source), by its 1-based lines.
    struct Source {
        std::string content;
        std::optional<Text> text;
    };

    llvm::DenseMap<std::uint32_t, std::unique_ptr<Source>> sources;
    auto text_of = [&](std::uint32_t file) -> const Text& {
        auto& source = sources[file];
        if(!source) {
            source = std::make_unique<Source>();
            if(auto content = vfs::read_text(table.resolve(fids[file]))) {
                source->content = std::move(*content);
            }
            source->text.emplace(source->content);
        }
        return *source->text;
    };
    // The code tokens of 1-based `line` of `file`.
    auto tokens_on = [&](std::uint32_t file, std::uint32_t line) -> llvm::ArrayRef<Token> {
        auto& text = text_of(file);
        return line != 0 && line <= text.lines.size() ? text.tokens_on(line - 1)
                                                      : llvm::ArrayRef<Token>();
    };
    // Whether `line` of `file` writes the identifier `word` in its code.
    auto writes = [&](std::uint32_t file, std::uint32_t line, llvm::StringRef word) {
        auto& text = text_of(file);
        return line != 0 && line <= text.lines.size() && text.names(line - 1, word);
    };
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

    // A directive's text, its continuation lines joined.
    auto directive_text = [&](std::uint32_t file, std::uint32_t line) {
        auto& text = text_of(file);
        return line != 0 && line <= text.lines.size() && !text.keywords[line - 1].empty()
                   ? text.directive_text(line - 1)
                   : std::string();
    };

    // A name qualified by its containers, inline namespaces skipped as
    // lookup skips them. A TU-local container is in `shard`, the shard
    // holding the entity's entries.
    auto find_symbol = [&](index::SymbolHash hash, const index::Shard* shard) {
        auto identity = index.find_symbol(hash);
        if(!identity && shard) {
            identity = shard->find_symbol(hash);
        }
        return identity;
    };
    auto qualified_name = [&](index::SymbolHash hash, const index::Shard* shard) {
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
    };

    // The name a module interface exports an entity by: its own at
    // namespace scope; an unscoped enum's enumerator there by the enclosing
    // namespace, which the enum declares it in.
    auto export_name_of = [&](index::SymbolHash hash,
                              const index::SymbolView& identity,
                              const index::Shard* shard) -> std::string {
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
    };

    // What each scoped file names, from its reference relations: a spelled name
    // mirrors its occurrence there, and a name a macro expansion produces
    // has only the relation. A weak relation is one candidate of a dependent call's
    // overload set, kept apart: the candidates an operator finds where a
    // template is defined are not what its instantiations call.
    llvm::DenseMap<index::SymbolHash, llvm::SmallVector<Site, 2>> sites;
    std::vector<llvm::DenseMap<index::SymbolHash, RawUse>> raw(facts.files.size());
    std::vector<llvm::DenseSet<std::uint32_t>> pasted_lines(facts.files.size());
    std::vector<std::vector<std::pair<std::uint32_t, index::SymbolHash>>> weak_sites(
        facts.files.size());
    /// (file, entity) -> the units whose variant of the file declares it,
    /// for a file read differently across units.
    llvm::DenseMap<std::pair<std::uint32_t, index::SymbolHash>, llvm::SmallVector<std::uint32_t>>
        declaring_units;
    /// (file, entity) declared by only some of the file's variants.
    llvm::DenseSet<std::pair<std::uint32_t, index::SymbolHash>> partially_declared;
    std::vector<std::vector<std::pair<std::uint32_t, index::SymbolHash>>> use_sites(
        facts.files.size());
    /// Specialization, primary template, file.
    std::vector<std::tuple<index::SymbolHash, index::SymbolHash, std::uint32_t>>
        raw_specializations;
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
                           RelationKind::WeakReference,
                           RelationKind::Read,
                           RelationKind::Write,
                           RelationKind::Pasted);
        };
        auto declaration_of = [](const index::Relation& relation) {
            return RelationKind(relation.kind).isDeclOrDef();
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
                use.strong |= relation.kind != RelationKind::WeakReference;
                use_sites[id].emplace_back(relation.range.begin, hash);
                if(relation.kind == RelationKind::WeakReference) {
                    weak_sites[id].emplace_back(relation.range.begin, hash);
                }
            }
        });
    }

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

    // A macro definition some #undef removes is a helper scoped to the lines
    // between (an X-macro around a table): the #undef's relation references it.
    // A header included again undefines its own macro ahead of defining it
    // anew (<assert.h>, <limits.h>): that #undef ends nothing.
    llvm::DenseSet<index::SymbolHash> undefined;
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

    // Macro name -> the (file, line) of each scoped definition.
    llvm::StringMap<llvm::SmallVector<std::pair<std::uint32_t, std::uint32_t>, 1>>
        macro_definitions;
    for(auto& [hash, list]: sites) {
        if(auto identity = find_symbol(hash, index.shard(fids[list.front().file]));
           identity && identity->kind == SymbolKind::Macro) {
            for(auto& site: list) {
                macro_definitions[identity->name].emplace_back(site.file, site.line);
            }
        }
    }

    // A friend declaration, its keyword on a line of the declaration up to
    // the name: from the extent's first line, or with none the line before
    // when the name opens its own; or in a macro those lines expand, as
    // toml++ spells its comparison friends.
    auto friend_site = [&](const Site& site, llvm::StringRef name) {
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
                for(auto [file, at]:
                    macro_definitions.lookup(token.text(text_of(site.file).content))) {
                    for_each_identifier(directive_text(file, at), [&](llvm::StringRef word) {
                        found = found || word == "friend";
                    });
                }
            }
        }
        return found;
    };

    // The namespace a position of a file sits in, from the extents of the
    // declarations there, `self`'s own apart: "" for the global one, none
    // inside a class or a function.
    std::vector<std::vector<std::pair<LocalSourceRange, index::SymbolHash>>> definitions(
        facts.files.size());
    for(auto& [hash, list]: sites) {
        for(auto& site: list) {
            if(site.extent.end > site.extent.begin) {
                definitions[site.file].emplace_back(site.extent, hash);
            }
        }
    }
    auto namespace_at = [&](std::uint32_t file,
                            std::uint32_t offset,
                            index::SymbolHash self) -> std::optional<std::string> {
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
    };

    // A using-declaration at namespace scope (`namespace hlsl { using
    // dxil::ResourceClass; }`) re-declares a name there: a weak relation of the
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

    // Every hash some scoped file names or declares, classified once.
    llvm::DenseMap<index::SymbolHash, std::uint32_t> entity_ids;
    llvm::DenseMap<index::SymbolHash, std::pair<Fid, std::string>> foreign_macros;
    std::vector<index::SymbolHash> parents;
    std::vector<bool> operators;
    auto classify = [&](index::SymbolHash hash) {
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
                    foreign_macros[hash] = {canonical, identity->name.str()};
                    return;
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
                return;
            }
        }
        if(owner == none && scoped.empty()) {
            return;
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
        // A fragment's entities belong to the file pasting it in: the one
        // whose declaration encloses the paste when the fragment fills a
        // declaration (clang's Options.inc filling clang's enum in clang and
        // the program's enum in the program), else the first by path.
        auto declared_in = owner;
        if(facts.files[owner].fragment) {
            auto includers = including_files(facts, owner, true);
            if(includers.empty()) {
                return;
            }
            // A site in a fragment stands for the files including it.
            auto stand_for = [&](std::uint32_t file, std::uint32_t includer) {
                return ranges::contains(including_files(facts, file, true), includer);
            };
            auto narrow = [&](auto keeps) {
                auto kept = includers;
                kept.erase(
                    ranges::remove_if(kept,
                                      [&](std::uint32_t includer) { return !keeps(includer); })
                        .begin(),
                    kept.end());
                if(!kept.empty()) {
                    includers = std::move(kept);
                }
            };
            if(auto enclosing = sites.find(identity->parent); enclosing != sites.end()) {
                narrow([&](std::uint32_t includer) {
                    return ranges::any_of(enclosing->second, [&](const Site& site) {
                        return stand_for(site.file, includer);
                    });
                });
            }
            // The includer defining the X-macro the table's line expands, a
            // level deep (OMPConstants.h defines OMP_DEFAULT_KIND, which
            // OMPKinds.def's __OMP_DEFAULT_KIND(none) expands to a constant).
            if(auto site = ranges::find(scoped, declared_in, &Site::file);
               site != scoped.end() && includers.size() > 1) {
                llvm::StringSet<> names;
                for(const auto& token: tokens_on(declared_in, site->line)) {
                    if(token.is_identifier()) {
                        names.insert(token.text(text_of(declared_in).content));
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
                        return ranges::any_of(macro_definitions.lookup(word),
                                              [&](auto& definition) {
                                                  return definition.first != declared_in &&
                                                         stand_for(definition.first, includer);
                                              });
                    });
                });
            }
            owner = *ranges::min_element(includers, [&](std::uint32_t lhs, std::uint32_t rhs) {
                return facts.files[lhs].path < facts.files[rhs].path;
            });
        }
        auto* owner_shard = index.shard(fids[declared_in]);
        auto site = ranges::find(scoped, declared_in, &Site::file);
        auto line = site != scoped.end() ? site->line : 0;

        auto linkage = InternalLinkage::None;
        if(internal) {
            linkage =
                kind == SymbolKind::Variable ? InternalLinkage::Const : InternalLinkage::Static;
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
        bool constant = kind == SymbolKind::Variable && (writes(declared_in, line, "const") ||
                                                         writes(declared_in, line, "constexpr"));

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
            .name = kind == SymbolKind::Macro ? identity->name.str()
                                              : qualified_name(hash, owner_shard),
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
            .directive =
                kind == SymbolKind::Macro ? directive_text(declared_in, line) : std::string(),
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
    };
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
    auto entity_of = [&](index::SymbolHash hash) {
        auto it = entity_ids.find(hash);
        return it == entity_ids.end() ? none : it->second;
    };

    // A member's top is the outermost container its owner also provides.
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
                closure.emplace();
                llvm::SmallVector<Fid> pending{fids[id]};
                while(!pending.empty()) {
                    auto file = pending.pop_back_val();
                    if(closure->insert(file).second) {
                        if(auto it = includes.find(file); it != includes.end()) {
                            pending.append(it->second.begin(), it->second.end());
                        }
                    }
                }
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

    // Each use relation inside a definition the file provides belongs to the
    // innermost such definition: what moves with it.
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
        for(auto& site: list) {
            if(site.definition && site.extent.end > site.extent.begin &&
               facts.entities[entity].owner == site.file) {
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
            if(body.empty() || body.back().entity != used) {
                body.push_back({.entity = used});
            }
            body.back().count += 1;
        }
    }

    // A header naming a macro it does not include the definition of reads
    // differently compiled alone. The closure is over every include edge,
    // guard-skipped ones too: a skipped directive still names the file.
    // Fragments stay textual by design and read their includer's macros.
    for(std::uint32_t id = 0; id < facts.files.size(); id += 1) {
        if(facts.files[id].source || facts.files[id].fragment) {
            continue;
        }
        llvm::DenseSet<Fid> closure;
        llvm::SmallVector<Fid> pending{fids[id]};
        while(!pending.empty()) {
            auto file = pending.pop_back_val();
            if(!closure.insert(file).second) {
                continue;
            }
            if(auto it = includes.find(file); it != includes.end()) {
                pending.append(it->second.begin(), it->second.end());
            }
        }
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
    return facts;
}

std::expected<LoadedFacts, LoadError> load_facts(const Spelling& workspace,
                                                 llvm::StringRef configuration,
                                                 llvm::ArrayRef<std::string> scope,
                                                 FactsEdit edit) {
    std::vector<kota::GlobPattern> globs;
    for(auto& pattern: scope) {
        auto glob = kota::GlobPattern::create(pattern);
        if(!glob) {
            return std::unexpected(LoadError{
                .message =
                    std::format("invalid --scope glob '{}': {}", pattern, glob.error().message),
            });
        }
        globs.push_back(std::move(*glob));
    }

    auto config = Config::load_from_workspace(CanonicalPath(workspace));
    if(auto checked = check_requested_configuration(config, configuration); !checked) {
        return std::unexpected(LoadError{.message = std::move(checked.error())});
    }
    auto reader =
        IndexReader::open(workspace, std::move(config), configuration, {.load = IndexLoad::Saved});
    if(!reader) {
        return std::unexpected(LoadError{.message = "no usable index; run `clice index` first"});
    }
    auto& project = reader->project;
    auto& files = reader->files;
    // A file written since it was indexed (as `clice refactor rename`
    // leaves the files it edits) would give facts about text that is gone.
    LoadError stale{.message =
                        "some files changed since they were indexed; run `clice index` first"};
    project.index.for_each_indexed_file([&](Fid file) {
        if(auto* shard = project.index.shard(file);
           shard && reader->gate.stale(file, shard->content_hash())) {
            stale.dropped.emplace_back(files.display(file));
        }
    });
    // A unit owed the variants a rebuild discarded is missing from them.
    for(auto unit: project.index.owed_units()) {
        stale.dropped.emplace_back(files.display(unit));
    }
    if(!stale.dropped.empty()) {
        ranges::sort(stale.dropped);
        return std::unexpected(std::move(stale));
    }

    auto facts = collect(project, [&](llvm::StringRef path) {
        if(!globs.empty()) {
            return ranges::any_of(globs, [&](auto& glob) { return glob.match(path); });
        }
        return !path::is_absolute(path) && !path.starts_with(".") && !path.contains("/.");
    });
    if(project.index.damaged()) {
        return std::unexpected(LoadError{.message = damaged_index_error(project)});
    }
    if(edit) {
        if(auto edited = edit(*reader, facts); !edited) {
            return std::unexpected(LoadError{.message = std::move(edited.error())});
        }
    }
    return LoadedFacts{.root = llvm::StringRef(project.config.workspace_root).str(),
                       .facts = std::move(facts)};
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

bool is_keyword(llvm::StringRef word) {
    return keyword_kind(
               word,
               keywords(raw_dialect(clang::Language::CXX, clang::LangStandard::lang_cxx23))) !=
           clang::tok::identifier;
}

bool is_module_name(llvm::StringRef name) {
    llvm::SmallVector<llvm::StringRef> parts;
    name.split(parts, '.');
    return ranges::all_of(parts, [](llvm::StringRef part) {
        return !part.empty() && (llvm::isAlpha(part.front()) || part.front() == '_') &&
               ranges::all_of(part, [](char c) { return llvm::isAlnum(c) || c == '_'; }) &&
               !is_keyword(part) && part != "module" && part != "import";
    });
}

std::expected<Partition, std::string> partition(const Facts& facts, const PartitionSpec& spec) {
    Partition result;
    llvm::StringMap<std::uint32_t> ids;
    auto module_id = [&](llvm::StringRef name) {
        auto [it, inserted] =
            ids.try_emplace(name, static_cast<std::uint32_t>(result.modules.size()));
        if(inserted) {
            result.modules.push_back(name.str());
        }
        return it->second;
    };

    std::vector<std::pair<llvm::StringRef, kota::GlobPattern>> globs;
    for(auto& module: spec.modules) {
        for(auto& pattern: module.files) {
            auto glob = kota::GlobPattern::create(pattern);
            if(!glob) {
                return std::unexpected(
                    std::format("invalid glob '{}': {}", pattern, glob.error().message));
            }
            globs.emplace_back(module.name, std::move(*glob));
        }
    }

    auto directory = [&](llvm::StringRef path) {
        llvm::StringRef dir(path::parent(path, path::Style::Posix));
        if(dir.empty()) {
            return std::string(".");
        }
        if(spec.depth == 0) {
            return dir.str();
        }
        llvm::SmallVector<llvm::StringRef> segments;
        dir.split(segments, '/');
        segments.truncate(std::min<std::size_t>(spec.depth, segments.size()));
        return llvm::join(segments, "/");
    };

    // The names whose glob matches a file; a directory module that happens to
    // share one stays the program's.
    llvm::StringSet<> matched_names;
    for(auto& file: facts.files) {
        auto matched =
            ranges::find_if(globs, [&](auto& glob) { return glob.second.match(file.path); });
        if(matched != globs.end()) {
            matched_names.insert(matched->first);
        }
        result.module_of.push_back(module_id(
            matched != globs.end() ? matched->first : llvm::StringRef(directory(file.path))));
    }

    for(llvm::StringRef move: spec.moves) {
        auto [path, module] = move.rsplit('=');
        auto it = facts.file_ids.find(path);
        if(module.empty() || it == facts.file_ids.end()) {
            return std::unexpected(std::format("--move {}: no scoped file {}",
                                               std::string_view(move),
                                               std::string_view(path)));
        }
        result.module_of[it->second] = module_id(module);
    }

    // A module merged away answers to the one it joined.
    std::vector<std::uint32_t> joined(result.modules.size());
    for(std::uint32_t module = 0; module < joined.size(); module += 1) {
        joined[module] = module;
    }
    for(llvm::StringRef merge: spec.merges) {
        llvm::SmallVector<llvm::StringRef> names;
        merge.split(names, '+');
        llvm::SmallVector<std::uint32_t> members;
        for(auto name: names) {
            auto it = ids.find(name);
            if(it == ids.end()) {
                return std::unexpected(std::format("--merge {}: no module {}",
                                                   std::string_view(merge),
                                                   std::string_view(name)));
            }
            auto module = it->second;
            while(joined[module] != module) {
                module = joined[module];
            }
            members.push_back(module);
        }
        for(auto member: members) {
            joined[member] = members.front();
        }
        for(auto& module: result.module_of) {
            if(ranges::contains(members, module)) {
                module = members.front();
            }
        }
    }

    // Modules a move or merge emptied leave the table.
    std::vector<std::uint32_t> remap(result.modules.size(), none);
    std::vector<std::string> modules;
    for(auto& module: result.module_of) {
        if(remap[module] == none) {
            remap[module] = static_cast<std::uint32_t>(modules.size());
            modules.push_back(result.modules[module]);
        }
        module = remap[module];
    }
    result.modules = std::move(modules);

    auto count = result.modules.size();
    result.kinds.assign(count, ModuleKind::Program);
    result.provides.resize(count);
    result.primaries.resize(count);
    // Whether an entry rewrites the module, and whether one wraps it, as an
    // entry without a kind says.
    std::vector<bool> rewritten(count, false), wrapped(count, false);
    for(auto& declared: spec.modules) {
        // A merge or move can still take a matched module's files away.
        auto module = result.module_named(declared.name);
        if(!matched_names.contains(declared.name) || module == count) {
            continue;
        }
        // Wrapped is what an entry without a kind says; another entry of
        // the module may name its kind.
        auto& kind = result.kinds[module];
        if(declared.kind == ModuleKind::Wrapped) {
            wrapped[module] = true;
            if(kind == ModuleKind::Program) {
                kind = ModuleKind::Wrapped;
            }
        } else if(kind == ModuleKind::Program || kind == ModuleKind::Wrapped) {
            kind = declared.kind;
        } else if(kind != declared.kind) {
            return std::unexpected(
                std::format("module {} is both textual and external", declared.name));
        }
        for(auto& provided: declared.provides) {
            result.provides[module].insert(provided.getKey());
        }
        if(declared.rewrite) {
            rewritten[module] = true;
            if(!declared.primary.empty()) {
                result.primaries[module] = declared.primary;
            }
        }
    }

    // A rewritten module's primary interface defaults to the deepest
    // directory holding its headers, its files' when it has none.
    for(std::uint32_t module = 0; module < count; module += 1) {
        if(rewritten[module] && (wrapped[module] || result.kinds[module] != ModuleKind::Program)) {
            return std::unexpected(
                std::format("module {} is both rewritten and wrapped", result.modules[module]));
        }
        auto& primary = result.primaries[module];
        if(!rewritten[module] || !primary.empty()) {
            continue;
        }
        std::optional<llvm::StringRef> common;
        for(bool headers: {true, false}) {
            for(std::uint32_t file = 0; file < facts.files.size(); file += 1) {
                auto& info = facts.files[file];
                if(result.module_of[file] != module || (headers && info.source)) {
                    continue;
                }
                auto dir = path::parent(info.path, path::Style::Posix);
                if(!common) {
                    common = dir;
                }
                while(!common->empty() && !path::is_within(dir, *common)) {
                    common = path::parent(*common, path::Style::Posix);
                }
            }
            if(common) {
                break;
            }
        }
        primary = common.value_or("").str();
        path::push(primary, "module.cppm", path::Style::Posix);
    }

    // A rewritten module writes its files in place and declares its name.
    llvm::StringMap<std::uint32_t> placed;
    for(std::uint32_t module = 0; module < count; module += 1) {
        auto& primary = result.primaries[module];
        if(primary.empty()) {
            continue;
        }
        if(!is_module_name(result.modules[module])) {
            return std::unexpected(
                std::format("module {}: not a module name to rewrite", result.modules[module]));
        }
        if(auto [it, inserted] = placed.try_emplace(primary, module); !inserted) {
            auto [first, second] = std::minmax(result.modules[it->second], result.modules[module]);
            return std::unexpected(
                std::format("modules {} and {} both put their primary interface at {}",
                            first,
                            second,
                            primary));
        }
    }
    for(std::uint32_t file = 0; file < facts.files.size(); file += 1) {
        auto module = result.module_of[file];
        if(!result.primaries[module].empty() &&
           path::is_absolute(facts.files[file].path, path::Style::Posix)) {
            return std::unexpected(std::format("module {} would rewrite {}, outside the workspace",
                                               result.modules[module],
                                               facts.files[file].path));
        }
    }
    return result;
}

namespace {

/// Strongly connected components by Tarjan's algorithm: a component is
/// emitted after every component it reaches, so the result lists
/// dependencies first.
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

/// The uses read backwards: who names each entity, what each file owns.
struct Reverse {
    /// Entity -> the scoped files naming it, a fragment's names charged to
    /// the files pasting it in; never the owner itself.
    std::vector<llvm::SmallVector<std::uint32_t, 4>> users;

    /// File -> the entities it owns, macros apart.
    std::vector<llvm::SmallVector<std::uint32_t>> owned;

    explicit Reverse(const Facts& facts) : users(facts.entities.size()), owned(facts.files.size()) {
        for(std::uint32_t file = 0; file < facts.files.size(); file += 1) {
            for(auto charged: charged_files(facts, file)) {
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
};

/// The module-level view of the facts under one partition.
struct Graph {
    const Facts& facts;
    const Partition& partition;

    struct Edge {
        /// Entity -> the from-module's files naming it.
        std::map<std::uint32_t, llvm::SmallVector<std::uint32_t, 2>> entities;

        /// The entities a header of the from-module names.
        llvm::DenseSet<std::uint32_t> interface;
    };

    std::map<std::pair<std::uint32_t, std::uint32_t>, Edge> edges;

    /// Module -> the modules its headers name, which its interface would
    /// import.
    std::vector<llvm::SmallVector<std::uint32_t>> imports;

    /// Strongly connected components over `imports`, dependencies first.
    std::vector<llvm::SmallVector<std::uint32_t>> components;
    std::vector<std::uint32_t> component_of;
    std::vector<std::uint32_t> layer;

    Reverse reverse;

    /// (file, entity) pairs where the file declares the entity without
    /// defining it: inside one module the file needs only its own
    /// declaration, not the header providing the entity.
    llvm::DenseSet<std::pair<std::uint32_t, std::uint32_t>> forward_declared;

    /// File -> the scoped files (charged for fragments) naming what it
    /// owns, apart from those of its module that declare it themselves.
    std::vector<llvm::SmallVector<std::uint32_t>> dependents;

    /// Headers only their own module's sources use, directly or through
    /// other such headers: internal partitions, whose names never reach
    /// the module's interface. A header specializing an out-of-scope
    /// template has users no relation shows and never is one.
    std::vector<bool> internal;

    Graph(const Facts& facts, const Partition& partition) :
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
        for(std::uint32_t file = 0; file < facts.files.size(); file += 1) {
            auto& info = facts.files[file];
            internal[file] = !info.source && !info.fragment && info.specializes.empty() &&
                             !dependents[file].empty() &&
                             ranges::all_of(dependents[file], [&](std::uint32_t user) {
                                 return partition.module_of[user] == partition.module_of[file];
                             });
        }
        // A header the module's interface uses cannot stay internal.
        for(bool changed = true; changed;) {
            changed = false;
            for(std::uint32_t file = 0; file < facts.files.size(); file += 1) {
                if(internal[file] && ranges::any_of(dependents[file], [&](std::uint32_t user) {
                       return !facts.files[user].source && !internal[user];
                   })) {
                    internal[file] = false;
                    changed = true;
                }
            }
        }

        imports.resize(count);
        for(std::uint32_t file = 0; file < facts.files.size(); file += 1) {
            for(auto user: charged_files(facts, file)) {
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

    std::uint32_t weight(std::uint32_t from, std::uint32_t to) const {
        auto it = edges.find({from, to});
        return it == edges.end() ? 0 : static_cast<std::uint32_t>(it->second.interface.size());
    }

    std::uint32_t cyclic_modules() const {
        std::uint32_t count = 0;
        for(auto& component: components) {
            if(component.size() > 1) {
                count += static_cast<std::uint32_t>(component.size());
            }
        }
        return count;
    }

    /// The lightest set of edges whose removal leaves `members` acyclic,
    /// as (from, to) pairs. Exact over every bottom-up order for small
    /// components; larger ones place the module importing the least of
    /// the rest lowest, one at a time.
    llvm::SmallVector<std::pair<std::uint32_t, std::uint32_t>>
        cut(llvm::ArrayRef<std::uint32_t> members) const {
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
};

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

/// File -> the fragments it pastes in, whose uses are charged to it.
std::vector<llvm::SmallVector<std::uint32_t, 1>> pasted_fragments(const Facts& facts) {
    std::vector<llvm::SmallVector<std::uint32_t, 1>> pasted(facts.files.size());
    for(std::uint32_t file = 0; file < facts.files.size(); file += 1) {
        if(facts.files[file].fragment) {
            for(auto charged: charged_files(facts, file)) {
                pasted[charged].push_back(file);
            }
        }
    }
    return pasted;
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

/// Rebuild weights of an edit to each file: today's from the units
/// entering it, the partition's from the importer closure of its module.
struct Rebuild {
    std::vector<double> baseline;
    std::vector<double> partitioned;

    Rebuild(const Graph& graph, const Annotations& annotations) {
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
        // The sources reaching each internal partition, through other
        // internal partitions too.
        auto reaching = [&](std::uint32_t header) {
            llvm::SmallVector<std::uint32_t> sources;
            llvm::DenseSet<std::uint32_t> reached{header};
            llvm::SmallVector<std::uint32_t> pending{header};
            while(!pending.empty()) {
                auto current = pending.pop_back_val();
                for(auto dependent: graph.dependents[current]) {
                    if(!reached.insert(dependent).second) {
                        continue;
                    }
                    if(facts.files[dependent].source) {
                        sources.push_back(dependent);
                    } else {
                        pending.push_back(dependent);
                    }
                }
            }
            return sources;
        };

        // The modules each source names beyond its own, itself or through
        // the internal partitions it reaches.
        std::vector<llvm::SmallDenseSet<std::uint32_t, 8>> named(facts.files.size());
        for(std::uint32_t file = 0; file < facts.files.size(); file += 1) {
            for(auto user: charged_files(facts, file)) {
                llvm::SmallVector<std::uint32_t> sources;
                if(facts.files[user].source) {
                    sources.push_back(user);
                } else if(graph.internal[user]) {
                    sources = reaching(user);
                }
                for(auto source: sources) {
                    for(auto& use: facts.uses[file]) {
                        named[source].insert(module_of[facts.entities[use.entity].owner]);
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
            for(auto user: charged_files(facts, file)) {
                if(facts.files[user].source) {
                    rebuilt.insert(user);
                } else if(!graph.internal[user]) {
                    rebuilt.insert_range(rebuilt_by_module[module_of[user]]);
                } else {
                    rebuilt.insert_range(reaching(user));
                }
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
};

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
        for(auto header: charged_files(facts, file)) {
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
    auto pasted = pasted_fragments(facts);
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
    auto pasted = pasted_fragments(facts);
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
    auto pasted = pasted_fragments(facts);
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

std::vector<Unit> Report::units() const {
    Graph graph(facts, partition);
    auto pasted = pasted_fragments(facts);

    // A header owning nothing other files name, an umbrella over other
    // headers, is internal too: its includers reach what it includes
    // through those headers' partitions. An internal header an interface
    // header or another module includes is none: an interface importing an
    // implementation partition leaves its names unreachable to importers.
    auto internal = graph.internal;
    for(std::uint32_t file = 0; file < facts.files.size(); file += 1) {
        auto& info = facts.files[file];
        if(!info.source && !info.fragment && info.specializes.empty() &&
           graph.dependents[file].empty()) {
            internal[file] = true;
        }
    }
    for(bool changed = true; changed;) {
        changed = false;
        for(std::uint32_t file = 0; file < facts.files.size(); file += 1) {
            if(internal[file] && ranges::any_of(facts.files[file].includers, [&](std::uint32_t by) {
                   return partition.module_of[by] != partition.module_of[file] ||
                          (!facts.files[by].source && !facts.files[by].fragment && !internal[by]);
               })) {
                internal[file] = false;
                changed = true;
            }
        }
    }

    std::vector<Unit> result(facts.files.size());
    for(std::uint32_t file = 0; file < facts.files.size(); file += 1) {
        auto& info = facts.files[file];
        auto& unit = result[file];
        unit.kind = info.source      ? Unit::Kind::Source
                    : info.fragment  ? Unit::Kind::Fragment
                    : internal[file] ? Unit::Kind::Internal
                                     : Unit::Kind::Interface;
        for(auto named: llvm::concat<const std::uint32_t>(llvm::ArrayRef(file), pasted[file])) {
            for(auto [uses, owners]: {
                    std::pair{&facts.uses[named],       &unit.names },
                    std::pair{&facts.macro_uses[named], &unit.macros}
            }) {
                for(auto& use: *uses) {
                    auto owner = facts.entities[use.entity].owner;
                    if(owner != file) {
                        owners->push_back(owner);
                    }
                }
            }
        }
        for(auto* owners: {&unit.names, &unit.macros}) {
            ranges::sort(*owners);
            owners->erase(ranges::unique(*owners).begin(), owners->end());
        }
    }
    return result;
}

std::expected<std::vector<Interface>, std::string> Report::interface(llvm::StringRef name) const {
    auto count = partition.modules.size();
    auto wanted = partition.module_named(name);
    if(!name.empty() && wanted == count) {
        return std::unexpected(std::format("no module {}", name.str()));
    }
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
        for(auto charged: charged_files(facts, file)) {
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
            for(auto file: charged_files(facts, redeclaration.file)) {
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
        for(auto file: including_files(facts, declaration.file, true)) {
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
        auto use = [&](const Use& named) {
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
        if(!name.empty() && module != wanted) {
            continue;
        }
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
                if(ranges::any_of(including_files(facts, declaration.file, true),
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
            auto charged = charged_files(facts, alias.file);
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

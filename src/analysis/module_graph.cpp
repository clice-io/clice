module;

#include "modules/prelude.h"

module clice;

import :analysis.module_graph;
import :command.command;
import :index.serialization;
import :project.project;
import :syntax.lexer;
import :vfs.file_system;
import :vfs.path;

namespace clice::analysis {

namespace {

constexpr std::uint32_t none = ~0u;

/// 1-based line of `offset` in the content a shard's rows describe.
std::uint32_t line_of(const index::Shard& shard, std::uint32_t offset) {
    auto starts = shard.line_starts();
    return static_cast<std::uint32_t>(std::ranges::upper_bound(starts, offset) - starts.begin());
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

    /// Some row names it outright rather than as an overload candidate.
    bool strong = false;
};

/// Whether `text` holds `word` as a whole identifier.
bool has_word(llvm::StringRef text, llvm::StringRef word) {
    for(auto at = text.find(word); at != llvm::StringRef::npos; at = text.find(word, at + 1)) {
        auto before = at == 0 ? ' ' : text[at - 1];
        auto after = at + word.size() < text.size() ? text[at + word.size()] : ' ';
        if(!llvm::isAlnum(before) && before != '_' && !llvm::isAlnum(after) && after != '_') {
            return true;
        }
    }
    return false;
}

/// Whether a block comment `text` opens runs past its end, literals and a
/// line comment aside.
bool opens_comment(llvm::StringRef text) {
    for(std::size_t at = 0; at < text.size(); at += 1) {
        auto c = text[at];
        if(c == '"' || c == '\'') {
            for(at += 1; at < text.size() && text[at] != c; at += 1) {
                at += text[at] == '\\' ? 1 : 0;
            }
        } else if(c == '/' && at + 1 < text.size() && text[at + 1] == '/') {
            return false;
        } else if(c == '/' && at + 1 < text.size() && text[at + 1] == '*') {
            auto close = text.find("*/", at + 2);
            if(close == llvm::StringRef::npos) {
                return true;
            }
            at = close + 1;
        }
    }
    return false;
}

/// `text` with its string and character literals emptied and its comments
/// cut: the code words of a line.
std::string code_of(llvm::StringRef text) {
    std::string result;
    for(std::size_t at = 0; at < text.size(); at += 1) {
        auto c = text[at];
        if(c == '/' && at + 1 < text.size() && text[at + 1] == '/') {
            break;
        }
        if(c == '/' && at + 1 < text.size() && text[at + 1] == '*') {
            auto close = text.find("*/", at + 2);
            if(close == llvm::StringRef::npos) {
                break;
            }
            result += ' ';
            at = close + 1;
            continue;
        }
        result += c;
        if(c != '"' && c != '\'') {
            continue;
        }
        for(at += 1; at < text.size() && text[at] != c; at += 1) {
            at += text[at] == '\\' ? 1 : 0;
        }
        result += c;
    }
    return result;
}

/// Visit each identifier of `text`.
void for_each_identifier(llvm::StringRef text, llvm::function_ref<void(llvm::StringRef)> visit) {
    while(true) {
        auto start = text.find_if([](char c) { return llvm::isAlpha(c) || c == '_'; });
        if(start == llvm::StringRef::npos) {
            return;
        }
        text = text.drop_front(start);
        auto word = text.take_while([](char c) { return llvm::isAlnum(c) || c == '_'; });
        visit(word);
        text = text.drop_front(word.size());
    }
}

/// The identifiers a macro definition's replacement list spells: neither
/// the macro's own name nor its parameters, which name nothing outside it.
void for_each_expanded(llvm::StringRef directive, llvm::function_ref<void(llvm::StringRef)> visit) {
    auto text = directive.ltrim();
    text.consume_front("#");
    text = text.ltrim();
    text.consume_front("define");
    text = text.ltrim().drop_while([](char c) { return llvm::isAlnum(c) || c == '_'; });
    llvm::SmallVector<llvm::StringRef> parameters;
    if(text.consume_front("(")) {
        auto list = text.take_until([](char c) { return c == ')'; });
        text = text.drop_front(list.size());
        text.consume_front(")");
        for_each_identifier(list, [&](llvm::StringRef name) { parameters.push_back(name); });
    }
    for_each_identifier(code_of(text), [&](llvm::StringRef word) {
        if(!llvm::is_contained(parameters, word)) {
            visit(word);
        }
    });
}

/// Whether `text` declares `name` a namespace alias: `namespace name =`.
bool declares_alias(llvm::StringRef text, llvm::StringRef name) {
    for(auto at = text.find("namespace"); at != llvm::StringRef::npos;
        at = text.find("namespace", at + 1)) {
        auto rest = text.drop_front(at + llvm::StringRef("namespace").size()).ltrim();
        if(rest.consume_front(name) && rest.ltrim().starts_with("=")) {
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
/// Pasted rows, and its own rows merge what every pasting context made of
/// it (clang's Options.inc expands to enumerators in clang and to option
/// tables naming the program's types in the program).
llvm::SmallVector<std::uint32_t> charged_files(const Facts& facts, std::uint32_t file) {
    return including_files(facts, file, false);
}

}  // namespace

Facts collect(Project& project, llvm::function_ref<bool(llvm::StringRef)> in_scope) {
    Facts facts;
    auto& index = project.project_index;
    auto& table = project.file_table;
    llvm::StringRef root(index.workspace);

    // A path relative to the workspace, empty for one outside it.
    auto relative_of = [&](Fid fid) {
        llvm::StringRef identity(table.resolve(fid));
        if(identity.size() <= root.size() || !path::under(identity, root)) {
            return std::string();
        }
        return identity.drop_front(root.size()).ltrim('/').str();
    };
    auto display = [&](Fid fid) {
        auto relative = relative_of(fid);
        return relative.empty() ? std::string(table.display(fid)) : relative;
    };

    // The scoped files are the indexed ones: those holding rows, the
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
            .source = index.manifests.contains(fid) && !is_header_path(path),
            .fragment = is_context_header_path(path),
        });
        fids.push_back(fid);
    };
    for(auto& entry: index.shards) {
        add(entry.first);
    }
    for(auto& [tu, manifest]: index.manifests) {
        add(tu);
        for(auto& node: manifest.nodes) {
            add(table.version(VersionID{node.file}).fid);
        }
    }
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
    /// (file, rows hash) -> the scoped units whose rows of the file are
    /// that variant.
    llvm::DenseMap<std::pair<std::uint32_t, std::uint64_t>, llvm::SmallVector<std::uint32_t, 2>>
        contributors;
    /// (includer, directive line) -> the file the directive includes.
    llvm::DenseMap<std::pair<std::uint32_t, std::uint32_t>, Fid> include_at;
    /// (scoped includer, scoped file) -> the first directive line naming it.
    llvm::DenseMap<std::pair<std::uint32_t, std::uint32_t>, std::uint32_t> include_lines;
    for(auto& [tu, manifest]: index.manifests) {
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
            for(auto& [version, rows]: manifest.contributions) {
                if(auto id = id_of(table.version(version).fid); id != none) {
                    contributors[{id, rows}].push_back(unit);
                }
            }
        }
    }
    for(auto& file: facts.files) {
        std::ranges::sort(file.directives);
        file.directives.erase(std::ranges::unique(file.directives).begin(), file.directives.end());
    }
    for(auto& [includer, targets]: includes) {
        std::ranges::sort(targets);
        targets.erase(std::ranges::unique(targets).begin(), targets.end());
        auto id = id_of(includer);
        for(auto target: targets) {
            included_by[target].push_back(includer);
            if(auto included = id_of(target); id != none && included != none && included != id) {
                facts.files[id].includes.push_back(included);
                facts.files[included].includers.push_back(id);
            }
        }
    }

    // The text of a scoped file's line, from the disk: the rows hold
    // positions, not the source.
    llvm::DenseMap<std::uint32_t, std::string> contents;
    auto text_of = [&](std::uint32_t file) -> llvm::StringRef {
        auto [it, inserted] = contents.try_emplace(file);
        if(inserted) {
            if(auto buffer = vfs::read(table.resolve(fids[file]))) {
                it->second = (*buffer)->getBuffer().str();
            }
        }
        return it->second;
    };
    auto line_text = [&](std::uint32_t file, std::uint32_t line) {
        auto text = text_of(file);
        for(std::uint32_t current = 1; current < line && !text.empty(); current += 1) {
            text = text.split('\n').second;
        }
        return text.split('\n').first;
    };
    for(std::uint32_t id = 0; id < facts.files.size(); id += 1) {
        auto& file = facts.files[id];
        for(auto includer: file.includers) {
            // A forced include (-include) has no directive in the includer;
            // its presumed line points at some other text there.
            auto directive = line_text(includer, include_lines.lookup({includer, id})).ltrim();
            auto open = directive.find_first_of("<\"");
            auto close = open == llvm::StringRef::npos
                             ? open
                             : directive.find(directive[open] == '<' ? '>' : '"', open + 1);
            auto spelled = close == llvm::StringRef::npos ? llvm::StringRef()
                                                          : directive.slice(open, close + 1);
            auto names_file =
                !spelled.empty() &&
                llvm::sys::path::filename(spelled.drop_front().drop_back(),
                                          llvm::sys::path::Style::posix) ==
                    llvm::sys::path::filename(file.path, llvm::sys::path::Style::posix);
            file.spellings.push_back(directive.starts_with("#") && directive.contains("include") &&
                                             names_file
                                         ? spelled.str()
                                         : std::string());
        }
    }

    // A directive's text, its continuation lines joined.
    // A block comment the directive opens runs on to the line closing it:
    // replayed cut short, it would swallow what follows.
    auto directive_text = [&](std::uint32_t file, std::uint32_t line) {
        std::string text;
        for(auto current = line; current <= facts.files[file].lines; current += 1) {
            auto part = line_text(file, current).rtrim('\r');
            if(part.ends_with("\\")) {
                text += part.drop_back();
                continue;
            }
            text += part;
            if(!opens_comment(text)) {
                break;
            }
            text += ' ';
        }
        return text;
    };

    // A name qualified by its containers, inline namespaces skipped as
    // lookup skips them. A TU-local container is in `shard`, the shard
    // holding the entity's rows.
    auto identity_of = [&](index::SymbolHash hash, const index::Shard* shard) {
        auto identity = index.identity_of(hash);
        if(!identity && shard) {
            identity = shard->find_symbol(hash);
        }
        return identity;
    };
    auto qualified_name = [&](index::SymbolHash hash, const index::Shard* shard) {
        auto identity = identity_of(hash, shard);
        if(!identity) {
            return std::string();
        }
        auto name = index::SymbolRef::from(hash, *identity).display_name();
        // A persisted parent column can be cyclic; the tables only reject
        // reserved values.
        llvm::DenseSet<index::SymbolHash> visited{hash};
        for(auto parent = identity->parent; parent != 0 && visited.insert(parent).second;) {
            auto scope = identity_of(parent, shard);
            if(!scope) {
                break;
            }
            if(!index::transparent_scope(scope->flags)) {
                name = index::SymbolRef::from(parent, *scope).display_name() + "::" + name;
            }
            parent = scope->parent;
        }
        return name;
    };

    // The name a module interface exports an entity by: its own at
    // namespace scope; an unscoped enum's enumerator there by the enclosing
    // namespace, which the enum declares it in.
    auto export_name_of = [&](index::SymbolHash hash,
                              const index::SymbolIdentity& identity,
                              const index::Shard* shard) -> std::string {
        // A deduction guide has no name to export it by.
        if(identity.kind == SymbolKind::Macro ||
           index::has_flag(identity.flags, index::SymbolFlags::Specialization) ||
           index::name_form(identity.flags) == index::NameForm::Other) {
            return {};
        }
        auto parent = identity.parent != 0 ? identity_of(identity.parent, shard) : std::nullopt;
        if(!parent || parent->kind == SymbolKind::Namespace) {
            return index::has_flag(identity.flags, index::SymbolFlags::Unnamed)
                       ? std::string()
                       : qualified_name(hash, shard);
        }
        if(identity.kind != SymbolKind::EnumMember || parent->kind != SymbolKind::Enum ||
           !index::has_flag(identity.flags, index::SymbolFlags::Completable)) {
            return {};
        }
        auto grand = parent->parent != 0 ? identity_of(parent->parent, shard) : std::nullopt;
        if(grand && grand->kind != SymbolKind::Namespace) {
            return {};
        }
        auto enclosing = qualified_name(identity.parent, shard);
        auto separator = llvm::StringRef(enclosing).rfind("::");
        return (separator == llvm::StringRef::npos ? std::string()
                                                   : enclosing.substr(0, separator + 2)) +
               identity.name.str();
    };

    // What each scoped file names, from its reference rows: a spelled name
    // mirrors its occurrence there, and a name a macro expansion produces
    // has only the row. A weak row is one candidate of a dependent call's
    // overload set, kept apart: the candidates an operator finds where a
    // template is defined are not what its instantiations call.
    llvm::DenseMap<index::SymbolHash, llvm::SmallVector<Site, 2>> sites;
    std::vector<llvm::DenseMap<index::SymbolHash, RawUse>> raw(facts.files.size());
    std::vector<llvm::DenseSet<std::uint32_t>> pasted_lines(facts.files.size());
    std::vector<std::vector<std::pair<std::uint32_t, index::SymbolHash>>> weak_rows(
        facts.files.size());
    /// (file, entity) -> the units whose variant of the file declares it,
    /// for a file read differently across units.
    llvm::DenseMap<std::pair<std::uint32_t, index::SymbolHash>, llvm::SmallVector<std::uint32_t>>
        declaring_units;
    std::vector<std::vector<std::pair<std::uint32_t, index::SymbolHash>>> use_rows(
        facts.files.size());
    /// Specialization, primary template, file.
    std::vector<std::tuple<index::SymbolHash, index::SymbolHash, std::uint32_t>>
        raw_specializations;
    for(std::uint32_t id = 0; id < facts.files.size(); id += 1) {
        auto shard_it = index.shards.find(fids[id]);
        if(shard_it == index.shards.end() || !shard_it->second.loaded()) {
            continue;
        }
        auto& shard = shard_it->second;
        auto names =
            [&](llvm::function_ref<void(index::SymbolHash, const index::Relation&)> visit) {
                shard.for_each_relation(
                    [&](index::SymbolHash hash, const index::Relation& relation) {
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
            llvm::DenseMap<index::SymbolHash, std::uint32_t> carried;
            std::optional<llvm::DenseSet<index::SymbolHash>> declared;
            for(auto variant: live) {
                shard.set_live({variant});
                llvm::DenseSet<index::SymbolHash> named, declares;
                names([&](index::SymbolHash hash, const index::Relation& relation) {
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
                    auto& units = declaring_units[{id, hash}];
                    units.append(contributors.lookup({id, variant}));
                }
                declared = std::move(declares);
            }
            shard.set_live(live);
            auto& unstable = facts.files[id].unstable;
            for(auto& [hash, count]: carried) {
                if(count != live.size()) {
                    unstable.push_back(qualified_name(hash, &shard));
                }
            }
            std::ranges::sort(unstable);
            unstable.erase(std::ranges::unique(unstable).begin(), unstable.end());
        }

        names([&](index::SymbolHash hash, const index::Relation& relation) {
            if(declaration_of(relation)) {
                sites[hash].push_back({
                    .file = id,
                    .line = line_of(shard, relation.range.begin),
                    .offset = relation.range.begin,
                    .definition = relation.kind == RelationKind::Definition,
                    .extent = relation.definition_range(),
                });
            } else if(relation.kind == RelationKind::Primary) {
                raw_specializations.emplace_back(hash, relation.target_symbol, id);
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
                use_rows[id].emplace_back(relation.range.begin, hash);
                if(relation.kind == RelationKind::WeakReference) {
                    weak_rows[id].emplace_back(relation.range.begin, hash);
                }
            }
        });
    }

    // A fragment pasted inside a declaration: the includer's Pasted rows at
    // the directive name what the fragment names there.
    for(std::uint32_t id = 0; id < facts.files.size(); id += 1) {
        for(auto line: pasted_lines[id]) {
            auto it = include_at.find({fids[id].raw, line});
            if(it == include_at.end()) {
                continue;
            }
            if(auto fragment = id_of(it->second);
               fragment != none && facts.files[fragment].fragment &&
               !llvm::is_contained(facts.files[fragment].pasters, id)) {
                facts.files[fragment].pasters.push_back(id);
            }
        }
    }

    // A macro definition some #undef removes is a helper scoped to the lines
    // between (an X-macro around a table): the #undef's row references it.
    // A header included again undefines its own macro ahead of defining it
    // anew (<assert.h>, <limits.h>): that #undef ends nothing.
    llvm::DenseSet<index::SymbolHash> undefined;
    for(std::uint32_t id = 0; id < facts.files.size(); id += 1) {
        for(auto& [offset, hash]: use_rows[id]) {
            if(undefined.contains(hash)) {
                continue;
            }
            auto* shard = index.shard(fids[id]);
            auto identity = identity_of(hash, shard);
            if(!identity || identity->kind != SymbolKind::Macro) {
                continue;
            }
            auto line = line_of(*shard, offset);
            auto text =
                text_of(id).substr(shard->line_starts()[line - 1]).split('\n').first.ltrim();
            if(!text.consume_front("#") || !text.ltrim().starts_with("undef")) {
                continue;
            }
            // Elsewhere, an #undef is one file's cleanup (a consumer dropping
            // <windows.h>'s min), not the end of the macro.
            auto here = sites.lookup(hash);
            auto scoped = facts.files[id].fragment || llvm::any_of(here, [&](const Site& site) {
                              return site.file == id && site.line < line;
                          });
            auto redefined = llvm::any_of(here, [&](const Site& site) {
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
        if(auto identity = identity_of(hash, index.shard(fids[list.front().file]));
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
        } else if(first > 1 && line_text(site.file, first).ltrim().starts_with(name)) {
            first -= 1;
        }
        bool found = false;
        for(auto line = first; line <= site.line && !found; line += 1) {
            auto text = code_of(line_text(site.file, line));
            found = has_word(text, "friend");
            for_each_identifier(text, [&](llvm::StringRef word) {
                for(auto [file, at]: macro_definitions.lookup(word)) {
                    found = found || has_word(code_of(directive_text(file, at)), "friend");
                }
            });
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
        auto enclosing = identity_of(innermost->second, shard);
        if(!enclosing || enclosing->kind != SymbolKind::Namespace) {
            return std::nullopt;
        }
        return qualified_name(innermost->second, shard);
    };

    // A using-declaration at namespace scope (`namespace hlsl { using
    // dxil::ResourceClass; }`) re-declares a name there: a weak row of the
    // target on a `using` line in a namespace.
    for(std::uint32_t id = 0; id < facts.files.size(); id += 1) {
        auto* shard = index.shard(fids[id]);
        for(auto& [offset, target]: weak_rows[id]) {
            auto identity = identity_of(target, shard);
            if(!identity || identity->name.empty()) {
                continue;
            }
            auto text = line_text(id, line_of(*shard, offset));
            if(!has_word(text, "using") || has_word(text, "namespace") ||
               !text.contains(("::" + identity->name).str())) {
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

    // A namespace alias at namespace scope: its definition row, and on its
    // line the last row naming a namespace, the one it aliases.
    for(auto& [hash, list]: sites) {
        auto identity = index.identity_of(hash);
        if(!identity || identity->kind != SymbolKind::Namespace) {
            continue;
        }
        auto parent = identity->parent != 0 ? index.identity_of(identity->parent) : std::nullopt;
        if(parent && parent->kind != SymbolKind::Namespace) {
            continue;
        }
        auto site = std::ranges::find_if(list, [&](const Site& site) {
            return !facts.files[site.file].source &&
                   declares_alias(line_text(site.file, site.line), identity->name);
        });
        if(site == list.end()) {
            continue;
        }
        auto* shard = index.shard(fids[site->file]);
        index::SymbolHash target = 0;
        std::uint32_t last = 0;
        for(auto& [offset, used]: use_rows[site->file]) {
            if(used == hash || offset < last || line_of(*shard, offset) != site->line) {
                continue;
            }
            if(auto named = identity_of(used, shard);
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

        std::optional<index::SymbolIdentity> identity = index.identity_of(hash);
        bool internal = !identity;
        for(std::size_t i = 0; !identity && i < scoped.size(); i += 1) {
            if(auto* shard = index.shard(fids[scoped[i].file])) {
                identity = shard->find_symbol(hash);
            }
        }
        if(!identity || identity->scope == index::SymbolScope::FileLocal ||
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
            bool implemented =
                index.manifests.contains(canonical) && !llvm::is_contained({SymbolKind::Class,
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
                            .friend_declaration =
                                has_word(line_text(site.file, site.line), "friend"),
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
                return llvm::is_contained(including_files(facts, file, true), includer);
            };
            auto narrow = [&](auto keeps) {
                auto kept = includers;
                llvm::erase_if(kept, [&](std::uint32_t includer) { return !keeps(includer); });
                if(!kept.empty()) {
                    includers = std::move(kept);
                }
            };
            if(auto enclosing = sites.find(identity->parent); enclosing != sites.end()) {
                narrow([&](std::uint32_t includer) {
                    return llvm::any_of(enclosing->second, [&](const Site& site) {
                        return stand_for(site.file, includer);
                    });
                });
            }
            // The includer defining the X-macro the table's line expands, a
            // level deep (OMPConstants.h defines OMP_DEFAULT_KIND, which
            // OMPKinds.def's __OMP_DEFAULT_KIND(none) expands to a constant).
            if(auto site = std::ranges::find(scoped, declared_in, &Site::file);
               site != scoped.end() && includers.size() > 1) {
                llvm::StringSet<> names;
                for_each_identifier(line_text(declared_in, site->line),
                                    [&](llvm::StringRef word) { names.insert(word); });
                for(auto& word: llvm::to_vector(names.keys())) {
                    for(auto [file, line]: macro_definitions.lookup(word)) {
                        for_each_identifier(directive_text(file, line),
                                            [&](llvm::StringRef inner) { names.insert(inner); });
                    }
                }
                narrow([&](std::uint32_t includer) {
                    return llvm::any_of(names.keys(), [&](llvm::StringRef word) {
                        return llvm::any_of(macro_definitions.lookup(word), [&](auto& definition) {
                            return definition.first != declared_in &&
                                   stand_for(definition.first, includer);
                        });
                    });
                });
            }
            owner = *std::ranges::min_element(includers, [&](std::uint32_t lhs, std::uint32_t rhs) {
                return facts.files[lhs].path < facts.files[rhs].path;
            });
        }
        auto* owner_shard = index.shard(fids[declared_in]);
        auto site = std::ranges::find(scoped, declared_in, &Site::file);
        auto line = site != scoped.end() ? site->line : 0;

        auto linkage = InternalLinkage::None;
        if(internal) {
            linkage =
                kind == SymbolKind::Variable ? InternalLinkage::Const : InternalLinkage::Static;
            llvm::DenseSet<index::SymbolHash> visited{hash};
            for(auto parent = identity->parent; parent != 0 && visited.insert(parent).second;) {
                auto scope = identity_of(parent, owner_shard);
                if(!scope) {
                    break;
                }
                if(scope->kind == SymbolKind::Namespace &&
                   index::has_flag(scope->flags, index::SymbolFlags::Unnamed)) {
                    linkage = InternalLinkage::AnonymousNamespace;
                    break;
                }
                parent = scope->parent;
            }
            if(linkage == InternalLinkage::Const &&
               has_word(line_text(declared_in, line), "static")) {
                linkage = InternalLinkage::Static;
            }
        }
        bool constant =
            kind == SymbolKind::Variable && (has_word(line_text(declared_in, line), "const") ||
                                             has_word(line_text(declared_in, line), "constexpr"));

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
            .linkage = linkage,
            .top = id,
            .line = line,
            .self_uses = raw[owner].lookup(hash).count +
                         (declared_in != owner ? raw[declared_in].lookup(hash).count : 0),
            .constant = constant,
            .units =
                [&] {
                    auto units = declaring_units.lookup({declared_in, hash});
                    std::ranges::sort(units);
                    units.erase(std::ranges::unique(units).begin(), units.end());
                    return std::vector<std::uint32_t>(units.begin(), units.end());
                }(),
            // A hidden friend has no name lookup finds; argument dependent
            // lookup reaches it through its class.
            .export_name = llvm::is_contained({SymbolKind::Function,
                                               SymbolKind::Operator,
                                               SymbolKind::Class,
                                               SymbolKind::Struct,
                                               SymbolKind::Union},
                                              kind) &&
                                   llvm::all_of(scoped,
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
                index.each_reference_file(hash, [&](Fid file) {
                    if(id_of(file) != none || index.manifests.contains(file)) {
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
               !llvm::is_contained(defining_headers, site.file)) {
                defining_headers.push_back(site.file);
            }
            if(site.file != owner && site.file != declared_in) {
                facts.redeclarations.push_back({
                    .entity = id,
                    .file = site.file,
                    .line = site.line,
                    .definition = site.definition,
                    .friend_declaration = has_word(line_text(site.file, site.line), "friend"),
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
        return it != sites.end() && llvm::any_of(it->second, [&](const Site& site) {
                   if(site.file != file) {
                       return false;
                   }
                   // From the name to the body: the arguments and the bases.
                   auto text = text_of(file).substr(site.offset).take_until([](char c) {
                       return c == '{' || c == ';';
                   });
                   return llvm::any_of(owned[file], [&](std::uint32_t entity) {
                       // The specialization and its members are no arguments.
                       if(facts.entities[entity].hash == specialization ||
                          parents[entity] == specialization) {
                           return false;
                       }
                       llvm::StringRef name = facts.entities[entity].name;
                       auto separator = name.rfind("::");
                       return has_word(
                           text,
                           separator == llvm::StringRef::npos ? name : name.substr(separator + 2));
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
               !name.empty() && !llvm::is_contained(facts.files[file].specializes, name)) {
                facts.files[file].specializes.push_back(std::move(name));
            }
            continue;
        }
        if(facts.entities[entity].owner == file) {
            continue;
        }
        if(llvm::none_of(facts.specializations, [&](const Specialization& known) {
               return known.primary == entity && known.file == file;
           })) {
            std::uint32_t line = 0;
            if(auto it = sites.find(specialization); it != sites.end()) {
                if(auto site = llvm::find_if(it->second,
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
        return it != sites.end() && llvm::any_of(it->second, [&](const Site& site) {
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
            return it != sites.end() && llvm::any_of(it->second, [&](const Site& site) {
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
            std::ranges::sort(*list, {}, &Use::entity);
        }
        std::ranges::sort(facts.files[id].specializes);
    }

    // Each use row inside a definition the file provides belongs to the
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
        std::ranges::sort(definitions, [](const Extent& lhs, const Extent& rhs) {
            return std::tuple(lhs.begin, rhs.end) < std::tuple(rhs.begin, lhs.end);
        });
        auto& rows = use_rows[id];
        std::ranges::sort(rows);
        std::vector<std::pair<std::uint32_t, std::uint32_t>> owned_rows;
        llvm::SmallVector<const Extent*> open;
        std::size_t next = 0;
        for(auto& [offset, hash]: rows) {
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
            // Only rows the uses or self-uses count.
            auto& use = raw[id][hash];
            if(facts.entities[used].owner == id ||
               (!defines(id, hash) && (use.strong || !operators[used]))) {
                owned_rows.emplace_back(open.back()->entity, used);
            }
        }
        std::ranges::sort(owned_rows);
        for(auto& [entity, used]: owned_rows) {
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
            for(auto& [offset, row]: use_rows[id]) {
                if(row != hash) {
                    continue;
                }
                auto line = line_of(*shard, offset);
                auto text = line_text(id, line).ltrim();
                if(text.consume_front("#")) {
                    auto directive = text.ltrim().take_while(llvm::isAlpha);
                    // Naming it there needs no definition.
                    if(directive == "define" || directive == "undef") {
                        continue;
                    }
                    if(directive.starts_with("if") || directive.starts_with("elif")) {
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

std::expected<void, std::string> move_entities(Facts& facts, llvm::StringRef spec) {
    auto [selector, path] = spec.rsplit('=');
    if(selector.empty() || path.empty()) {
        return std::unexpected(
            std::format("--move-entity {}: expected <name or #id>=<path>", std::string_view(spec)));
    }
    llvm::SmallVector<std::uint32_t> selected;
    auto hash = index::parse_symbol_id(selector);
    for(std::uint32_t entity = 0; entity < facts.entities.size(); entity += 1) {
        auto& info = facts.entities[entity];
        if(info.kind != SymbolKind::Macro && (hash ? info.hash == *hash : info.name == selector)) {
            selected.push_back(entity);
        }
    }
    if(selected.empty()) {
        return std::unexpected(std::format("--move-entity {}: no entity {}",
                                           std::string_view(spec),
                                           std::string_view(selector)));
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
        return llvm::is_contained(selected, facts.entities[entity].top);
    };
    llvm::SmallVector<std::uint32_t> moved;
    for(std::uint32_t entity = 0; entity < facts.entities.size(); entity += 1) {
        if(moves(entity) && facts.entities[entity].owner != target) {
            moved.push_back(entity);
        }
    }
    auto add = [](std::vector<Use>& uses, std::uint32_t entity, std::uint32_t count) {
        auto use = std::ranges::lower_bound(uses, entity, {}, &Use::entity);
        if(use == uses.end() || use->entity != entity) {
            use = uses.insert(use, {.entity = entity});
        }
        use->count += count;
    };
    auto remove = [](std::vector<Use>& uses, std::uint32_t entity, std::uint32_t count) {
        auto use = std::ranges::lower_bound(uses, entity, {}, &Use::entity);
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
            add(facts.uses[target], entity, count);
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
        auto behind = info.self_uses - carried[{info.owner, entity}];
        if(behind != 0) {
            add(facts.uses[info.owner], entity, behind);
        }
        auto& from_target = facts.uses[target];
        if(auto use = std::ranges::lower_bound(from_target, entity, {}, &Use::entity);
           use != from_target.end() && use->entity == entity) {
            inside += use->count;
            from_target.erase(use);
        }
        info.self_uses = inside;
    }
    for(auto entity: moved) {
        facts.entities[entity].owner = target;
    }
    return {};
}

bool is_keyword(llvm::StringRef word) {
    const static auto options = raw_dialect(clang::Language::CXX, clang::LangStandard::lang_cxx23);
    static clang::IdentifierTable table(options);
    return table.get(word).getTokenID() != clang::tok::identifier;
}

bool is_module_name(llvm::StringRef name) {
    llvm::SmallVector<llvm::StringRef> parts;
    name.split(parts, '.');
    return llvm::all_of(parts, [](llvm::StringRef part) {
        return !part.empty() && (llvm::isAlpha(part.front()) || part.front() == '_') &&
               llvm::all_of(part, [](char c) { return llvm::isAlnum(c) || c == '_'; }) &&
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
        auto dir = llvm::sys::path::parent_path(path, llvm::sys::path::Style::posix);
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

    // The names a glob claims a file for; a directory module that happens to
    // share one stays the program's.
    llvm::StringSet<> claimed_names;
    for(auto& file: facts.files) {
        auto claimed =
            std::ranges::find_if(globs, [&](auto& glob) { return glob.second.match(file.path); });
        if(claimed != globs.end()) {
            claimed_names.insert(claimed->first);
        }
        result.module_of.push_back(module_id(
            claimed != globs.end() ? claimed->first : llvm::StringRef(directory(file.path))));
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
            if(llvm::is_contained(members, module)) {
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
    for(auto& claimed: spec.modules) {
        // A merge or move can still take a claimed module's files away.
        auto module = result.module_named(claimed.name);
        if(!claimed_names.contains(claimed.name) || module == count) {
            continue;
        }
        // Wrapped is what an entry without a kind says; another entry of
        // the module may name its kind.
        auto& kind = result.kinds[module];
        if(claimed.kind == ModuleKind::Wrapped) {
            wrapped[module] = true;
            if(kind == ModuleKind::Program) {
                kind = ModuleKind::Wrapped;
            }
        } else if(kind == ModuleKind::Program || kind == ModuleKind::Wrapped) {
            kind = claimed.kind;
        } else if(kind != claimed.kind) {
            return std::unexpected(
                std::format("module {} is both textual and external", claimed.name));
        }
        for(auto& provided: claimed.provides) {
            result.provides[module].insert(provided.getKey());
        }
        if(claimed.rewrite) {
            rewritten[module] = true;
            if(!claimed.primary.empty()) {
                result.primaries[module] = claimed.primary;
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
                auto dir = llvm::sys::path::parent_path(info.path, llvm::sys::path::Style::posix);
                if(!common) {
                    common = dir;
                }
                while(!common->empty() && !path::under(dir, *common)) {
                    common = llvm::sys::path::parent_path(*common, llvm::sys::path::Style::posix);
                }
            }
            if(common) {
                break;
            }
        }
        llvm::SmallString<256> path(common.value_or(""));
        llvm::sys::path::append(path, llvm::sys::path::Style::posix, "module.cppm");
        primary = path.str().str();
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
           llvm::sys::path::is_absolute(facts.files[file].path, llvm::sys::path::Style::posix)) {
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
                           !llvm::is_contained(named, charged)) {
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
    /// template has users no row shows and never is one.
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
                    if(llvm::is_contained(dependents[file], user) ||
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
                             llvm::all_of(dependents[file], [&](std::uint32_t user) {
                                 return partition.module_of[user] == partition.module_of[file];
                             });
        }
        // A header the module's interface uses cannot stay internal.
        for(bool changed = true; changed;) {
            changed = false;
            for(std::uint32_t file = 0; file < facts.files.size(); file += 1) {
                if(internal[file] && llvm::any_of(dependents[file], [&](std::uint32_t user) {
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
                    if(!llvm::is_contained(files, user)) {
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
            std::ranges::reverse(order);
        } else {
            llvm::SmallVector<std::uint32_t> rest(members);
            while(!rest.empty()) {
                auto lowest = std::ranges::min_element(rest, {}, [&](std::uint32_t v) {
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
    std::ranges::stable_sort(ranked, std::ranges::greater{}, [](auto& entry) {
        return entry.second;
    });
    std::vector<std::string> names;
    for(auto& entry: ranked) {
        auto& name = facts.entities[entry.first].name;
        if(!llvm::is_contained(names, name)) {
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
    std::ranges::sort(names);
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
                    llvm::any_of(named[file], [&](std::uint32_t m) { return affected[m]; }))) {
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
            std::ranges::sort(sources);
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
    std::ranges::sort(result.modules, [](auto& lhs, auto& rhs) {
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
    std::ranges::sort(result.edges, [](auto& lhs, auto& rhs) {
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
        std::ranges::sort(cycle.modules);
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
            std::ranges::sort(cut.files);
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
                   !llvm::is_contained(header_uses[header], owner)) {
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
        std::ranges::sort(cycle.headers);
    }
    std::ranges::sort(result.partition_cycles, {}, &PartitionCycle::module);
    result.internal_headers = static_cast<std::uint32_t>(llvm::count(graph.internal, true));

    if(!graph.components.empty()) {
        // Down from the highest component through the import that sets
        // each one's layer.
        auto top = std::ranges::max_element(
            std::views::iota(std::uint32_t(0), static_cast<std::uint32_t>(graph.components.size())),
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
    // row shows and stays put.
    auto& reverse = graph.reverse;
    llvm::DenseMap<std::uint32_t, llvm::SmallVector<std::uint32_t, 2>> implemented_by, implements;
    for(auto& redeclaration: facts.redeclarations) {
        auto header = facts.entities[redeclaration.entity].owner;
        if(!redeclaration.definition || !facts.files[redeclaration.file].source ||
           facts.files[header].source) {
            continue;
        }
        if(auto& sources = implemented_by[header];
           !llvm::is_contained(sources, redeclaration.file)) {
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
        if(llvm::any_of(unit, [&](std::uint32_t member) {
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
                if(!llvm::is_contained(unit, owner)) {
                    exchanged[partition.module_of[owner]].insert(use.entity);
                }
            }
            for(auto entity:
                llvm::concat<const std::uint32_t>(reverse.owned[member], macros[member])) {
                for(auto user: reverse.users[entity]) {
                    if(!llvm::is_contained(unit, user)) {
                        exchanged[partition.module_of[user]].insert(entity);
                        user_modules.insert(partition.module_of[user]);
                    }
                }
            }
        }
        // Moving the whole module is a merge.
        if(exchanged.contains(home) || user_modules.size() != 1 ||
           llvm::count(partition.module_of, home) == unit.size()) {
            continue;
        }
        std::ranges::sort(unit, {}, [&](std::uint32_t member) {
            return std::tie(facts.files[member].source, facts.files[member].path);
        });
        auto target = *user_modules.begin();
        auto& move = result.moves.emplace_back();
        move.path = facts.files[unit.front()].path;
        move.from = partition.modules[home];
        move.to = partition.modules[target];
        for(auto member: llvm::drop_begin(unit)) {
            move.with.push_back(facts.files[member].path);
        }
        move.entities = static_cast<std::uint32_t>(exchanged[target].size());
    }
    std::ranges::sort(result.moves, [](auto& lhs, auto& rhs) {
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
            std::ranges::sort(members, {}, [&](std::uint32_t member) {
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
            std::ranges::sort(parts, [](auto& lhs, auto& rhs) {
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
    std::ranges::sort(result.splits, [](auto& lhs, auto& rhs) {
        return std::tuple(rhs.parts.size(), lhs.path) < std::tuple(lhs.parts.size(), rhs.path);
    });
    if(result.splits.size() > limit) {
        result.splits.resize(limit);
    }

    auto blocked = obstacles();
    result.obstacles = {
        .variant_headers = static_cast<std::uint32_t>(
            llvm::count_if(blocked.variant_headers, &VariantHeader::declarations_differ)),
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
            auto use = std::ranges::lower_bound(uses, entity, {}, &Use::entity);
            if(use != uses.end() && use->entity == entity) {
                return std::format("{}:{}", facts.files[file].path, use->line);
            }
        }
        return facts.files[user].path;
    };
    for(auto& [entity, files]: it->second.entities) {
        auto& entry = result.entities.emplace_back();
        entry.id = index::symbol_id(facts.entities[entity].hash);
        entry.entity = facts.entities[entity].name;
        entry.owner = facts.files[facts.entities[entity].owner].path;
        entry.interface = it->second.interface.contains(entity);
        for(auto file: files) {
            entry.users.push_back(line_in(file, entity));
        }
        std::ranges::sort(entry.users);
    }
    std::ranges::sort(result.entities, [](auto& lhs, auto& rhs) {
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
        std::ranges::sort(result, by_entities);
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
        std::ranges::sort(group.consumers);
        std::ranges::sort(group.headers);
        group.depends_on.assign(depends_on.begin(), depends_on.end());
    }
    std::ranges::sort(result.groups, [](auto& lhs, auto& rhs) {
        return std::tuple(rhs.consumers.size(), lhs.headers) <
               std::tuple(lhs.consumers.size(), rhs.headers);
    });
    std::ranges::sort(result.files, {}, &FileLinks::path);
    std::ranges::sort(result.internal_headers);
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
    std::ranges::sort(result.affinity, by_entities);
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
    std::ranges::sort(result.variant_headers, [](auto& lhs, auto& rhs) {
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
        std::ranges::sort(use.users);
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
        auto named = llvm::any_of(facts.uses[redeclaration.file], [&](const Use& use) {
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
        std::ranges::sort(entry.files);
    }
    std::ranges::sort(result.duplicate_definitions, {}, &DuplicateEntry::entity);

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
    std::ranges::sort(result.configuring_macros, {}, &ConfiguringEntry::macro);

    for(auto& file: facts.files) {
        if(!file.specializes.empty()) {
            result.implicit_providers.push_back(
                {.path = file.path, .specializes = file.specializes});
        }
    }
    std::ranges::sort(result.implicit_providers, {}, &ProviderEntry::path);

    auto by_file = [](auto& entries) {
        std::ranges::sort(entries, [](auto& lhs, auto& rhs) {
            return std::tie(lhs.file, lhs.line) < std::tie(rhs.file, rhs.line);
        });
    };
    by_file(result.context_macros);
    by_file(result.borrowed_macros);
    by_file(result.cross_module_redeclarations);
    by_file(result.foreign_declarations);
    by_file(result.split_definitions);
    by_file(result.specializations);
    std::ranges::sort(result.internal_uses, [](auto& lhs, auto& rhs) {
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
    std::ranges::sort(result, [](auto& lhs, auto& rhs) {
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
    std::ranges::sort(result, [](auto& lhs, auto& rhs) {
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
            if(internal[file] && llvm::any_of(facts.files[file].includers, [&](std::uint32_t by) {
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
            std::ranges::sort(*owners);
            owners->erase(std::ranges::unique(*owners).begin(), owners->end());
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
    auto reaches = [&](std::uint32_t user, std::uint32_t target, bool everywhere) {
        if(contextual(target) && !everywhere) {
            return user == target || llvm::is_contained(facts.files[user].includes, target);
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
    std::vector<std::map<std::pair<std::string, std::uint32_t>, bool>> exports(count);
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
           llvm::any_of(facts.files[file].includers,
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
        auto everywhere =
            info.units.empty() || info.units.size() == facts.files[info.owner].units.size();
        auto user = llvm::find_if(users, [&](std::uint32_t candidate) {
            return module_of(candidate) != module &&
                   partition.kinds[module_of(candidate)] != ModuleKind::External &&
                   (!kept || (!self_declared.contains({entity, candidate}) &&
                              !reaches(candidate, info.owner, everywhere)));
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
                exports[module_of(file)].try_emplace({declaration.name, declaration.file});
            }
        }
    }
    for(std::uint32_t entity = 0; entity < facts.entities.size(); entity += 1) {
        auto& info = facts.entities[entity];
        auto module = module_of(info.owner);
        if(info.kind != SymbolKind::Macro && info.linkage == InternalLinkage::None &&
           !info.export_name.empty() && reached[module].contains(info.owner) &&
           llvm::any_of(info.units.empty() ? facts.files[info.owner].units : info.units,
                        [&](std::uint32_t unit) { return module_of(unit) != module; })) {
            exports[module][{info.export_name, info.owner}] |=
                llvm::any_of(reverse.users[entity],
                             [&](std::uint32_t user) { return module_of(user) != module; });
        }
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
            if(in.skipped[node] || (in.unit != user && !llvm::is_contained(in.files, user))) {
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
                if(llvm::any_of(includers, [&](std::uint32_t includer) {
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
                auto on_chain = llvm::find_if(entries, [&](std::uint32_t file) {
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
        // The module's own name for a header no other module includes.
        llvm::StringRef inside;
        for(std::size_t i = 0; i < info.includers.size(); i += 1) {
            auto includer = info.includers[i];
            llvm::StringRef spelled = info.spellings[i];
            if(spelled.size() < 2) {
                continue;
            }
            auto name = spelled.drop_front().drop_back();
            if(spelled.starts_with("\"")) {
                llvm::SmallString<256> beside(
                    llvm::sys::path::parent_path(facts.files[includer].path,
                                                 llvm::sys::path::Style::posix));
                llvm::sys::path::append(beside, llvm::sys::path::Style::posix, name);
                llvm::sys::path::remove_dots(beside, true, llvm::sys::path::Style::posix);
                if(beside == info.path) {
                    continue;
                }
            }
            if(module_of(includer) == module) {
                if(inside.empty()) {
                    inside = spelled;
                }
                continue;
            }
            // By a name the include path finds, an angled one first.
            if(header.include == by_path ||
               (spelled.starts_with("<") && !llvm::StringRef(header.include).starts_with("<"))) {
                header.include = spelled;
            }
            if(!llvm::is_contained(header.names, name)) {
                header.names.push_back(name.str());
            }
        }
        if(header.include == by_path && !inside.empty()) {
            header.include = inside.str();
        }
        std::ranges::sort(header.names);
        return header;
    };

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
        std::ranges::sort(interface.imports);
        for(auto file: entries[module]) {
            interface.entries.push_back(header_of(module, file, {}));
        }
        std::ranges::sort(interface.entries, {}, &InterfaceHeader::file);
        std::map<std::uint32_t, std::string> included;
        for(auto header: textual[module]) {
            if(partition.kinds[module] != ModuleKind::Textual) {
                included.try_emplace(header);
                continue;
            }
            auto [entity, user] = because[module].lookup(header);
            auto reason =
                std::format("{} in {}", facts.entities[entity].name, facts.files[user].path);
            included.try_emplace(entry_of(module, header, user), reason);
        }
        for(auto& [file, reason]: included) {
            interface.textual.push_back(header_of(module, file, reason));
        }
        std::ranges::sort(interface.textual, {}, &InterfaceHeader::file);
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
        }
        for(auto& [key, used]: exports[module]) {
            interface.exports.push_back(
                {.name = key.first, .file = facts.files[key.second].path, .used = used});
        }
        for(auto& alias: facts.aliases) {
            auto charged = charged_files(facts, alias.file);
            if(llvm::any_of(charged, [&](std::uint32_t file) {
                   return module_of(file) == module && reached[module].contains(file);
               })) {
                interface.aliases.push_back({.name = alias.name, .target = alias.target});
            }
        }
        std::ranges::sort(interface.aliases, {}, &InterfaceAlias::name);

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
        std::ranges::sort(ordered, [&](std::uint32_t lhs, std::uint32_t rhs) {
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
        std::ranges::sort(interface.reads, [](auto& lhs, auto& rhs) {
            return std::tie(lhs.file, lhs.name) < std::tie(rhs.file, rhs.name);
        });
    }
    return result;
}

}  // namespace clice::analysis

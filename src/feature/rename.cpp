module clice;

import :feature.hierarchy;
import :feature.protocol;
import :feature.rename;
import :index.search;
import :index.site;
import :index.symbol_query;
import :semantic.identity;
import :syntax.lexer;
import :syntax.source_text;

namespace clice::feature {

namespace {

/// The offsets in `text[begin, end)` where `name` stands as a whole
/// identifier.
llvm::SmallVector<std::uint32_t, 1>
    spellings(llvm::StringRef text, std::uint32_t begin, std::uint32_t end, llvm::StringRef name) {
    llvm::SmallVector<std::uint32_t, 1> found;
    for(auto at = text.find(name, begin); at != llvm::StringRef::npos && at + name.size() <= end;
        at = text.find(name, at + 1)) {
        auto after = at + name.size();
        if((at == 0 || !is_identifier_char(text[at - 1])) &&
           (after == text.size() || !is_identifier_char(text[after]))) {
            found.push_back(static_cast<std::uint32_t>(at));
        }
    }
    return found;
}

/// The site of the `length` bytes at `offset` in `text`, for a token no
/// relation spans.
index::Site token_site(SourceText& text,
                       Fid file,
                       llvm::StringRef path,
                       std::uint32_t offset,
                       std::uint32_t length) {
    return *index::site_of(file, path, text.positions(), {offset, offset + length});
}

/// The line holding `offset`, trimmed, for a preview.
std::string preview_line(SourceText& text, std::uint32_t offset) {
    return index::line_at(text.positions(), offset).trim().str();
}

bool class_like(SymbolKind kind) {
    return kind == SymbolKind::Class || kind == SymbolKind::Struct || kind == SymbolKind::Union;
}

/// Whether an identifier names the symbol, so that a rename may change it.
bool renamable(SymbolKind kind) {
    switch(kind) {
        case SymbolKind::Namespace:
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
        case SymbolKind::Parameter:
        case SymbolKind::Label:
        case SymbolKind::Concept:
        case SymbolKind::Operator: return true;
        default: return false;
    }
}

bool function_like(SymbolKind kind) {
    return kind == SymbolKind::Function || kind == SymbolKind::Method;
}

std::string where(const index::Site& site) {
    return std::format("{}:{}", site.path, site.begin.line + 1);
}

/// Every symbol named `name` exactly, however many.
std::vector<index::IndexQuery::Located> all_named(const index::IndexQuery& query,
                                                  llvm::StringRef name) {
    index::SymbolQuery exact{.mode = index::SymbolQuery::Mode::Exact, .pattern = name.str()};
    for(std::uint32_t limit = 256;; limit *= 4) {
        auto hits = index::search(query, exact, limit);
        if(hits.size() < limit) {
            return hits;
        }
    }
}

/// The newest dialect of each language.
const clang::LangOptions& cxx() {
    return raw_dialect(clang::Language::CXX, clang::LangStandard::lang_cxx26);
}

const clang::LangOptions& c() {
    return raw_dialect(clang::Language::C, clang::LangStandard::lang_c23);
}

/// Why a rename cannot reach a macro whose `#define` the index does not
/// serve: the compile command or the compiler defines it (`-DNAME`,
/// `__GNUC__`), or the file defining it moved on since it was indexed.
std::string unplaced_macro(const index::SymbolInfo& macro) {
    if(index::has_flag(macro.flags, index::SymbolFlags::HasDefinition)) {
        return std::format("the file defining `{}` changed since it was indexed", macro.name);
    }
    return std::format(
        "the compile command or the compiler defines `{}`, which a rename cannot change",
        macro.name);
}

/// The rename target of a macro: the definitions of its name a rename of
/// it changes too (RenameTarget). A use resolving to one the command line
/// gives as well, which no rename can change, refuses it.
std::expected<RenameTarget, std::string> macro_target(const index::IndexQuery& query,
                                                      const index::IndexQuery::Located& named) {
    llvm::StringRef name = named.symbol.name;
    auto in_system_header = [&](const index::IndexQuery::Located& macro) {
        return index::has_flag(macro.symbol.flags, index::SymbolFlags::SystemHeader);
    };
    if(in_system_header(named)) {
        return std::unexpected(
            std::format("`{}` is defined in a system header", std::string_view(name)));
    }
    auto definitions = all_named(query, name);
    std::erase_if(definitions, [](const index::IndexQuery::Located& hit) {
        return hit.symbol.kind != SymbolKind::Macro;
    });

    RenameTarget target{.symbol = named};
    llvm::DenseSet<index::SymbolHash> seen{named.symbol.hash};
    // The files defining or using one of them.
    llvm::DenseSet<Fid> tied;
    std::vector<index::IndexQuery::Located> pending{named};
    while(!pending.empty()) {
        auto next = std::move(pending.back());
        pending.pop_back();
        for(auto kind: {RelationKind::Definition, RelationKind::Reference}) {
            for(auto& site: query.sites(next.symbol.hash, next.site.file, kind)) {
                tied.insert(site.file);
                auto cursor = query.symbol_at(site.file, site.range.begin);
                if(!cursor) {
                    continue;
                }
                for(auto other: cursor->symbols) {
                    auto info = query.symbol_info(other, site.file);
                    if(!info || info->kind != SymbolKind::Macro || info->name != name ||
                       !seen.insert(other).second) {
                        continue;
                    }
                    auto located = query.resolve(other, site.file);
                    if(!located) {
                        return std::unexpected(
                            std::format("`{}` at {} is, in some units, another macro: {}",
                                        std::string_view(name),
                                        where(site),
                                        unplaced_macro(*info)));
                    }
                    if(in_system_header(*located)) {
                        return std::unexpected(
                            std::format("`{}` at {} is also defined in a system header",
                                        std::string_view(name),
                                        where(site)));
                    }
                    pending.push_back(std::move(*located));
                }
            }
        }
        target.group.push_back(std::move(next));
        for(auto& definition: definitions) {
            if(tied.contains(definition.site.file) && seen.insert(definition.symbol.hash).second) {
                pending.push_back(definition);
            }
        }
    }
    return target;
}

}  // namespace

std::optional<std::string> invalid_identifier(llvm::StringRef name) {
    std::string text = name.str();
    Lexer lexer(text, cxx());
    auto token = lexer.advance();
    if(!token.is_identifier() || token.range.begin != 0 || token.range.end != text.size() ||
       !lexer.advance().is_eof()) {
        return std::format("`{}` is not an identifier", std::string_view(name));
    }
    for(auto* options: {&c(), &cxx()}) {
        if(keyword_kind(name, keywords(*options)) != clang::tok::identifier) {
            return std::format("`{}` is a keyword", std::string_view(name));
        }
    }
    return std::nullopt;
}

std::expected<RenameTarget, std::string> rename_target(const index::IndexQuery& serving,
                                                       const index::IndexQuery::Located& named) {
    auto& symbol = named.symbol;
    if(symbol.kind == SymbolKind::Macro) {
        return macro_target(serving, named);
    }
    auto query = serving.written_only();
    if(!renamable(symbol.kind)) {
        return std::unexpected(std::format("`{}` cannot be renamed", symbol.name));
    }
    if(index::has_flag(symbol.flags, index::SymbolFlags::Unnamed)) {
        return std::unexpected(std::format("{} has no name to rename", symbol.name));
    }

    auto root = named;
    switch(index::name_form(symbol.flags)) {
        case index::NameForm::Identifier: break;
        case index::NameForm::Constructor:
        case index::NameForm::Destructor: {
            auto owner = query.resolve(symbol.parent, named.site.file);
            if(!owner) {
                return std::unexpected(
                    std::format("the class of `{}` is not indexed", symbol.name));
            }
            root = std::move(*owner);
            break;
        }
        case index::NameForm::Other: {
            auto deduced =
                query.located_targets(symbol.hash, named.site.file, RelationKind::TypeDefinition);
            if(deduced.empty()) {
                return std::unexpected(std::format("`{}` cannot be renamed", symbol.name));
            }
            root = std::move(deduced.front());
            break;
        }
        case index::NameForm::Conversion:
        case index::NameForm::Operator:
        case index::NameForm::Literal: {
            return std::unexpected(
                std::format("`{}` is named by the language, not by an identifier", symbol.name));
        }
    }
    if(index::has_flag(root.symbol.flags, index::SymbolFlags::Specialization)) {
        auto primaries =
            query.located_targets(root.symbol.hash, root.site.file, RelationKind::Primary);
        if(!primaries.empty()) {
            root = std::move(primaries.front());
        }
    }
    if(index::has_flag(root.symbol.flags, index::SymbolFlags::SystemHeader)) {
        return std::unexpected(
            std::format("`{}` is declared in a system header", root.symbol.display_name()));
    }
    if(index::has_flag(root.symbol.flags, index::SymbolFlags::SpelledInMacro)) {
        return std::unexpected(std::format("every declaration of `{}` is spelled by a macro",
                                           root.symbol.display_name()));
    }

    RenameTarget target{.symbol = root};
    llvm::DenseSet<index::SymbolHash> seen;
    std::vector<index::IndexQuery::Located> pending{root};
    while(!pending.empty()) {
        auto next = std::move(pending.back());
        pending.pop_back();
        if(!seen.insert(next.symbol.hash).second) {
            continue;
        }
        auto follow = [&](RelationKind kind) {
            pending.append_range(query.located_targets(next.symbol.hash, next.site.file, kind));
        };
        follow(RelationKind::Specialization);
        if(class_like(next.symbol.kind)) {
            follow(RelationKind::Constructor);
            follow(RelationKind::Destructor);
            follow(RelationKind::Guide);
        } else if(next.symbol.kind == SymbolKind::Method &&
                  index::name_form(next.symbol.flags) == index::NameForm::Identifier) {
            // A destructor overriding another names its own class.
            follow(RelationKind::Interface);
            follow(RelationKind::Implementation);
        }
        target.group.push_back(std::move(next));
    }
    return target;
}

std::expected<CursorRename, std::string> rename_at(const index::IndexQuery& serving,
                                                   const index::IndexQuery::Cursor& resolved) {
    auto query = serving.written_only();
    // A name only instantiations resolve (`value.print()` in a template)
    // spells no symbol of its own.
    auto cursor = query.symbol_at(resolved.site.file, resolved.site.range.begin);
    if(!cursor) {
        return std::unexpected(
            "what the name here refers to differs from one instantiation or macro expansion to "
            "the next; rename from a declaration");
    }
    std::optional<RenameTarget> found;
    std::string refused;
    for(auto& named: query.resolve_at(*cursor)) {
        auto target = rename_target(serving, named);
        if(!target) {
            refused = std::move(target.error());
            continue;
        }
        if(found) {
            // A macro's definitions linked together are one rename.
            if(!ranges::contains(
                   found->group,
                   target->symbol.symbol.hash,
                   [](const index::IndexQuery::Located& member) { return member.symbol.hash; })) {
                return std::unexpected(
                    "the name here resolves to several symbols; rename from "
                    "one of their declarations");
            }
            continue;
        }
        found = std::move(*target);
    }
    if(!found && refused.empty()) {
        for(auto symbol: cursor->symbols) {
            if(auto info = query.symbol_info(symbol, cursor->site.file);
               info && info->kind == SymbolKind::Macro) {
                return std::unexpected(unplaced_macro(*info));
            }
        }
    }
    if(!found) {
        return std::unexpected(refused.empty() ? "no symbol here to rename" : refused);
    }

    auto& site = cursor->site;
    auto text = query.serving_text(site.file);
    if(!text) {
        return std::unexpected(std::format("{} changed since it was indexed", site.path));
    }
    llvm::StringRef name = found->symbol.symbol.name;
    auto offsets = spellings(*text, site.range.begin, site.range.end, name);
    if(offsets.empty()) {
        return std::unexpected(
            std::format("no name of `{}` is written here", std::string_view(name)));
    }
    SourceText source(*text, cxx());
    auto token = token_site(source, site.file, site.path, offsets.front(), name.size());
    return CursorRename{.target = std::move(*found), .token = std::move(token)};
}

namespace {

/// A file the index holds whose text moved on since it was indexed, and
/// the names of a rename, or of a batch, its text spells now, each with
/// whether some spelling stands in a preprocessor directive.
struct Outdated {
    Fid file;
    std::string path;
    llvm::StringMap<bool> spelled;
};

/// The outdated files (IndexQuery::outdated_files) a rename may edit that
/// spell any of `names` as an identifier now.
std::vector<Outdated> outdated_files(const index::IndexQuery& query,
                                     FileTable& files,
                                     const RenameScope& scope,
                                     const llvm::StringSet<>& names) {
    std::vector<Outdated> outdated;
    for(auto file: query.outdated_files()) {
        auto path = files.resolve(file);
        if(!scope.editable(path)) {
            continue;
        }
        auto text = scope.read(path);
        if(!text || ranges::none_of(names.keys(), [&](llvm::StringRef name) {
               return llvm::StringRef(*text).contains(name);
           })) {
            continue;
        }
        Outdated entry{.file = file, .path = files.display(file)};
        SourceText source(*text, cxx());
        bool directive = false;
        for(auto& token: source.lexical().tokens) {
            if(token.is_directive_hash()) {
                directive = true;
            } else if(token.is_eod()) {
                directive = false;
            }
            if(auto name = token.text(*text); token.is_identifier() && names.contains(name)) {
                entry.spelled[name] |= directive;
            }
        }
        if(!entry.spelled.empty()) {
            outdated.push_back(std::move(entry));
        }
    }
    return outdated;
}

RenamePlan plan_rename(const index::IndexQuery& serving,
                       FileTable& files,
                       const RenameTarget& target,
                       llvm::StringRef new_name,
                       const RenameScope& scope,
                       const RenameBatch& batch,
                       llvm::ArrayRef<Outdated> outdated) {
    auto& root = target.symbol.symbol;
    bool macro = root.kind == SymbolKind::Macro;
    // A rename edits names as the text spells them: a macro's also where
    // the expansions of a body resolve one to it.
    auto query = macro ? serving : serving.written_only();
    RenamePlan plan{.old_name = root.name, .new_name = new_name.str()};
    llvm::StringRef old_name = plan.old_name;
    if(new_name == old_name) {
        return plan;
    }
    if(auto invalid = invalid_identifier(new_name)) {
        plan.conflicts.push_back(std::move(*invalid));
        return plan;
    }
    if(macro && new_name == "defined") {
        plan.conflicts.push_back("`defined` cannot name a macro");
        return plan;
    }
    llvm::DenseSet<index::SymbolHash> group;
    for(auto& member: target.group) {
        group.insert(member.symbol.hash);
    }
    // Another rename of the batch giving a symbol the same name changes its
    // tokens alike: a token it shares with this rename's is no obstacle.
    auto renamed_alike = [&](index::SymbolHash symbol) {
        auto other = batch.renamed.find(symbol);
        return other != batch.renamed.end() && other->second == new_name;
    };

    // The text every entry of a file indexes, and its positions.
    struct Text {
        std::optional<std::string> text;
        std::optional<SourceText> source;
    };

    llvm::DenseMap<Fid, std::unique_ptr<Text>> texts;
    auto text_of = [&](Fid file) -> Text& {
        auto& slot = texts[file];
        if(!slot) {
            slot = std::make_unique<Text>();
            slot->text = query.serving_text(file);
            if(slot->text) {
                slot->source.emplace(*slot->text, cxx());
            }
        }
        return *slot;
    };

    // Another renamable symbol of the old name the token names too: a
    // variant of a shared header compiled under other definitions, or an
    // overload a dependent call leaves open. Changing the token changes
    // its use as well.
    auto shared_with = [&](Fid file, std::uint32_t offset) -> std::optional<index::SymbolHash> {
        auto cursor = query.symbol_at(file, offset);
        if(!cursor) {
            return std::nullopt;
        }
        for(auto symbol: cursor->symbols) {
            if(group.contains(symbol) || renamed_alike(symbol)) {
                continue;
            }
            if(auto info = query.symbol_info(symbol, file);
               info && info->name == old_name && renamable(info->kind)) {
                return symbol;
            }
        }
        return std::nullopt;
    };

    llvm::StringSet<> stale;
    std::map<std::pair<Fid, std::uint32_t>, RenameEdit> edits;
    std::set<std::pair<Fid, std::uint32_t>> held;
    auto note = [&](const index::Site& site, std::uint32_t offset, std::string reason) {
        if(!held.insert({site.file, offset}).second) {
            return;
        }
        auto& text = text_of(site.file);
        plan.unconfirmed.push_back({
            .site = token_site(*text.source, site.file, site.path, offset, old_name.size()),
            .reason = std::move(reason),
            .line = preview_line(*text.source, offset),
        });
    };
    auto add_edit = [&](const index::Site& site, std::uint32_t offset, bool heuristic) {
        auto file = site.file;
        auto& text = text_of(file);
        if(auto other = shared_with(file, offset)) {
            auto name = query.qualified_name(*other, file);
            if(auto renamed = batch.renamed.find(*other); renamed != batch.renamed.end()) {
                if(held.insert({file, offset}).second) {
                    auto token = token_site(*text.source, file, site.path, offset, old_name.size());
                    plan.conflicts.push_back(
                        std::format("the name at {} also refers to `{}`, which another rename "
                                    "makes `{}`",
                                    where(token),
                                    name,
                                    renamed->second));
                }
                return;
            }
            note(site,
                 offset,
                 std::format("the name here also refers to `{}`, under another build "
                             "configuration or as another candidate of a dependent call",
                             name));
            return;
        }
        auto [it, inserted] = edits.try_emplace(
            {file, offset},
            RenameEdit{.site = token_site(*text.source, file, site.path, offset, old_name.size()),
                       .heuristic = heuristic});
        if(!inserted) {
            it->second.heuristic &= heuristic;
        }
    };
    // The offsets of the old name a site spells; none when the file's index
    // is not current, or the site spells another name — punctuation (a
    // construction's paren), a keyword (the destructor `delete` runs), or
    // a macro's invocation, which a note lists.
    const auto& cxx_keywords = keywords(cxx());
    auto spelled_at = [&](const index::Site& site) -> llvm::SmallVector<std::uint32_t, 1> {
        auto& text = text_of(site.file);
        if(!text.text) {
            stale.insert(site.path);
            return {};
        }
        auto offsets = spellings(*text.text, site.range.begin, site.range.end, old_name);
        auto written = llvm::StringRef(*text.text).slice(site.range.begin, site.range.end);
        if(offsets.empty() && ranges::any_of(written, is_identifier_char) &&
           keyword_kind(written, cxx_keywords) == clang::tok::identifier) {
            note(site,
                 site.range.begin,
                 std::format("the name is written here as `{}`", std::string_view(written)));
        }
        return offsets;
    };

    // The files the rename's own entries lie in.
    llvm::DenseSet<Fid> tied;
    for(auto& member: target.group) {
        for(auto kind: {RelationKind::Definition,
                        RelationKind::Declaration,
                        RelationKind::Reference,
                        RelationKind::WeakReference}) {
            for(auto& site: query.sites(member.symbol.hash, member.site.file, kind)) {
                tied.insert(site.file);
                for(auto offset: spelled_at(site)) {
                    add_edit(site, offset, kind == RelationKind::WeakReference);
                }
            }
        }
    }
    if(macro) {
        // The macro's name where no definition resolves it: one the files
        // defining or using the macro spell is the macro's.
        auto unresolved = EntityTable::unresolved_macro(old_name);
        for(auto& site: query.sites(unresolved, target.symbol.site.file, RelationKind::Reference)) {
            if(!tied.contains(site.file)) {
                if(text_of(site.file).text) {
                    note(site,
                         site.range.begin,
                         "a directive names it where no #define of it reaches, or in a block "
                         "the preprocessor skips");
                }
                continue;
            }
            for(auto offset: spelled_at(site)) {
                add_edit(site, offset, false);
            }
        }
    } else {
        // What the instantiations of a template and the expansions of a
        // macro body resolve: those tokens name other symbols elsewhere.
        for(auto& member: target.group) {
            for(auto kind: {RelationKind::Reference, RelationKind::WeakReference}) {
                for(auto& site: serving.sites(member.symbol.hash, member.site.file, kind)) {
                    if(site.origin == index::SiteOrigin::Written || !text_of(site.file).text ||
                       edits.contains({site.file, site.range.begin})) {
                        continue;
                    }
                    note(site,
                         site.range.begin,
                         site.origin == index::SiteOrigin::Expansion
                             ? "inside a #define body, which names something else in another "
                               "expansion"
                             : "in a template, which names it only in some instantiations");
                }
            }
        }
    }

    llvm::DenseSet<Fid> edited;
    for(auto& [key, edit]: edits) {
        auto file = key.first;
        if(!edited.insert(file).second) {
            continue;
        }
        auto path = files.resolve(file);
        if(!scope.editable(path)) {
            plan.conflicts.push_back(
                std::format("the rename would change {}, which is not a source of the project "
                            "(a system header, a dependency or a generated file)",
                            edit.site.path));
            continue;
        }
        // The edits are offsets into the text the index was built from.
        auto current = scope.read(path);
        if(!current || *current != *text_of(file).text) {
            stale.insert(edit.site.path);
        }
    }
    // A local or a parameter is out of another file's reach but through a
    // macro it defines.
    bool function_local = root.scope == index::SymbolScope::FunctionLocal;
    for(auto& entry: outdated) {
        bool foreign = function_local && entry.file != target.symbol.site.file;
        for(auto name: {old_name, new_name}) {
            if(auto it = entry.spelled.find(name);
               it != entry.spelled.end() && (!foreign || it->second)) {
                stale.insert(entry.path);
            }
        }
    }
    if(scope.units_pending) {
        plan.warnings.push_back(
            "some units of the build are not indexed yet: what they name is not renamed");
    }

    // What the new name would collide with — among the symbols bearing it
    // once the batch is done: those named so that no rename of the batch
    // renames, and those its renames name so.
    std::vector<index::IndexQuery::Located> named;
    llvm::DenseSet<index::SymbolHash> listed;
    auto list = [&](index::IndexQuery::Located hit) {
        if(!batch.renamed.contains(hit.symbol.hash)) {
            named.push_back(std::move(hit));
        }
    };
    for(auto& hit: all_named(query, new_name)) {
        if(listed.insert(hit.symbol.hash).second) {
            list(std::move(hit));
        }
    }
    for(auto file: edited) {
        auto source = query.serving(file);
        if(!source) {
            continue;
        }
        source->shard->for_each_relation([&](index::SymbolHash hash, const index::Relation&) {
            if(!listed.insert(hash).second) {
                return true;
            }
            if(auto info = query.symbol_info(hash, file); info && info->name == new_name) {
                if(auto located = query.resolve(hash, file)) {
                    list(std::move(*located));
                }
            }
            return true;
        });
    }
    if(auto given = batch.named.find(new_name); given != batch.named.end()) {
        for(auto& hit: given->second) {
            if(!group.contains(hit.symbol.hash)) {
                named.push_back(hit);
            }
        }
    }
    auto reserved = [&](bool global) {
        // Both languages reserve `__x` and `_X` everywhere, and `_x` at
        // file and global namespace scope.
        if(new_name.contains("__") ||
           (new_name.starts_with("_") &&
            (global || (new_name.size() > 1 && clang::isUppercase(new_name[1]))))) {
            plan.warnings.push_back(
                std::format("`{}` is reserved for the implementation", std::string_view(new_name)));
        }
    };

    if(macro) {
        reserved(true);
        // A macro has no scope: it replaces every name it is spelled as.
        for(auto& other: named) {
            if(group.contains(other.symbol.hash)) {
                continue;
            }
            plan.conflicts.push_back(
                other.symbol.kind == SymbolKind::Macro
                    ? std::format("`{}` is a macro already ({})",
                                  std::string_view(new_name),
                                  where(other.site))
                    : std::format("`{}` names `{}` ({}), which the macro would replace",
                                  std::string_view(new_name),
                                  query.qualified_name(other.symbol.hash, other.site.file),
                                  where(other.site)));
        }
        auto tested = query.sites(EntityTable::unresolved_macro(new_name),
                                  target.symbol.site.file,
                                  RelationKind::Reference);
        if(!tested.empty()) {
            plan.conflicts.push_back(
                std::format("a directive tests `{}` ({}), which the macro "
                            "would define",
                            std::string_view(new_name),
                            where(tested.front())));
        }
    } else {
        // The scope a name is looked up in: an unscoped enumerator's is its
        // enum's.
        auto scope_of = [&](const index::SymbolInfo& symbol, Fid anchor) {
            if(symbol.kind == SymbolKind::EnumMember &&
               index::has_flag(symbol.flags, index::SymbolFlags::Completable)) {
                if(auto owner = query.symbol_info(symbol.parent, anchor)) {
                    return owner->parent;
                }
            }
            return symbol.parent;
        };
        auto anchor = target.symbol.site.file;
        auto home = scope_of(root, anchor);

        // The classes above and below the scope: a member of one hides or is
        // hidden by the renamed member, where a sibling's never meets it.
        llvm::DenseSet<index::SymbolHash> hierarchy;
        auto parent = query.symbol_info(home, anchor);
        if(parent && class_like(parent->kind)) {
            auto renamed = batch.renamed.find(home);
            if((renamed == batch.renamed.end() ? parent->name : renamed->second) == new_name) {
                plan.conflicts.push_back(std::format("`{}` names the class the member belongs to",
                                                     std::string_view(new_name)));
            }
            if(auto located = query.resolve(home, anchor)) {
                hierarchy.insert(located->symbol.hash);
                for(bool up: {true, false}) {
                    std::vector<index::IndexQuery::Located> pending{*located};
                    while(!pending.empty()) {
                        auto next = std::move(pending.back());
                        pending.pop_back();
                        auto types = type_hierarchy(query, next.symbol.hash, next.site.file, {});
                        for(auto& type: up ? types.supertypes : types.subtypes) {
                            if(hierarchy.insert(type.symbol.hash).second) {
                                pending.push_back(std::move(type));
                            }
                        }
                    }
                }
            }
        }
        bool local = parent && function_like(parent->kind);
        // A function's parameters and locals belong to the declaration whose
        // name precedes them most closely in their file: another declaration's
        // never meet the renamed symbol.
        auto declaration_of = [&](const index::Site& site) {
            std::optional<std::uint32_t> nearest;
            for(auto kind: {RelationKind::Definition, RelationKind::Declaration}) {
                for(auto& declared: query.sites(home, site.file, kind)) {
                    if(declared.file == site.file && declared.range.begin < site.range.begin &&
                       (!nearest || declared.range.begin > *nearest)) {
                        nearest = declared.range.begin;
                    }
                }
            }
            return std::pair(site.file, nearest);
        };
        std::pair<Fid, std::optional<std::uint32_t>> declaration;
        if(local) {
            declaration = declaration_of(target.symbol.site);
        }
        reserved(home == 0);

        for(auto& other: named) {
            auto& symbol = other.symbol;
            if(group.contains(symbol.hash)) {
                continue;
            }
            if(symbol.kind == SymbolKind::Macro) {
                plan.conflicts.push_back(
                    std::format("`{}` is a macro ({}): it would expand in place of the new name",
                                std::string_view(new_name),
                                where(other.site)));
                continue;
            }
            auto other_home = scope_of(symbol, other.site.file);
            if(other_home == home) {
                if(local && declaration_of(other.site) != declaration) {
                    continue;
                }
                if(function_like(symbol.kind) && function_like(root.kind)) {
                    // Overloads of the old name renamed alike stay overloads.
                    if(symbol.name == old_name) {
                        continue;
                    }
                    plan.warnings.push_back(
                        std::format("`{}` already names a function in the same scope ({}): the "
                                    "renamed function overloads it, which fails to build in C or "
                                    "when their parameters are the same",
                                    std::string_view(new_name),
                                    where(other.site)));
                } else if(local) {
                    // Other locals clash only within one block, and the index
                    // records no blocks.
                    if(symbol.kind == root.kind &&
                       (root.kind == SymbolKind::Parameter || root.kind == SymbolKind::Label)) {
                        plan.conflicts.push_back(
                            std::format("`{}` already names a {} of the same function ({})",
                                        std::string_view(new_name),
                                        root.kind == SymbolKind::Label ? "label" : "parameter",
                                        where(other.site)));
                    }
                } else if(symbol.kind == SymbolKind::Namespace &&
                          root.kind == SymbolKind::Namespace) {
                    plan.conflicts.push_back(std::format(
                        "a namespace `{}` already exists in the same scope ({}); merging namespaces "
                        "is not supported",
                        std::string_view(new_name),
                        where(other.site)));
                } else {
                    plan.conflicts.push_back(
                        std::format("`{}` is already declared in the same scope ({})",
                                    std::string_view(new_name),
                                    where(other.site)));
                }
                continue;
            }
            if(class_like(root.kind) && group.contains(other_home)) {
                plan.conflicts.push_back(std::format(
                    "`{}` is a member of the renamed class ({}), which may not share its "
                    "name",
                    std::string_view(new_name),
                    where(other.site)));
                continue;
            }
            if(hierarchy.contains(other_home)) {
                plan.conflicts.push_back(std::format(
                    "`{}` is a member of {} ({}), which the renamed member would hide or "
                    "be hidden by",
                    std::string_view(new_name),
                    query.qualified_name(other_home, other.site.file),
                    where(other.site)));
            }
        }
    }

    for(auto& [key, edit]: edits) {
        plan.edits.push_back(std::move(edit));
    }
    ranges::sort(plan.edits, [](const RenameEdit& lhs, const RenameEdit& rhs) {
        return std::tie(lhs.site.path, lhs.site.range.begin) <
               std::tie(rhs.site.path, rhs.site.range.begin);
    });
    ranges::sort(plan.unconfirmed, [](const RenameNote& lhs, const RenameNote& rhs) {
        return std::tie(lhs.site.path, lhs.site.range.begin) <
               std::tie(rhs.site.path, rhs.site.range.begin);
    });
    for(auto& entry: stale) {
        plan.stale.push_back(entry.getKey().str());
    }
    ranges::sort(plan.stale);
    return plan;
}

}  // namespace

RenamePlan plan_rename(const index::IndexQuery& query,
                       FileTable& files,
                       const RenameTarget& target,
                       llvm::StringRef new_name,
                       const RenameScope& scope,
                       const RenameBatch& batch) {
    llvm::StringSet<> names{target.symbol.symbol.name, new_name};
    return plan_rename(query,
                       files,
                       target,
                       new_name,
                       scope,
                       batch,
                       outdated_files(query, files, scope, names));
}

BatchPlan plan_renames(const index::IndexQuery& query,
                       FileTable& files,
                       llvm::ArrayRef<Renaming> renamings,
                       const RenameScope& scope) {
    // A renaming whose symbol an earlier one renames already is planned
    // as nothing: the earlier plan holds its edits.
    RenameBatch batch;
    std::vector<std::optional<std::string>> repeats(renamings.size());
    for(std::uint32_t i = 0; i < renamings.size(); i += 1) {
        auto& [target, new_name] = renamings[i];
        if(auto earlier = batch.renamed.find(target.symbol.symbol.hash);
           earlier != batch.renamed.end()) {
            repeats[i] = earlier->second;
            continue;
        }
        for(auto& member: target.group) {
            batch.renamed.try_emplace(member.symbol.hash, new_name);
            batch.named[new_name].push_back(member);
        }
    }

    llvm::StringSet<> names;
    for(auto& [target, new_name]: renamings) {
        names.insert(target.symbol.symbol.name);
        names.insert(new_name);
    }
    auto outdated = outdated_files(query, files, scope, names);

    BatchPlan result;
    for(std::uint32_t i = 0; i < renamings.size(); i += 1) {
        auto& [target, new_name] = renamings[i];
        auto& old_name = target.symbol.symbol.name;
        if(auto& earlier = repeats[i]) {
            auto& plan =
                result.plans.emplace_back(RenamePlan{.old_name = old_name, .new_name = new_name});
            if(*earlier == new_name) {
                plan.warnings.push_back(std::format("`{}` is renamed to `{}` already",
                                                    target.symbol.symbol.display_name(),
                                                    new_name));
            } else {
                plan.conflicts.push_back(std::format("`{}` is renamed to `{}` as well",
                                                     target.symbol.symbol.display_name(),
                                                     *earlier));
            }
            continue;
        }
        result.plans.push_back(plan_rename(query, files, target, new_name, scope, batch, outdated));
    }

    std::map<std::pair<std::string, std::uint32_t>, BatchEdit> edits;
    for(std::uint32_t i = 0; i < result.plans.size(); i += 1) {
        for(auto& edit: result.plans[i].edits) {
            auto [it, inserted] = edits.try_emplace({edit.site.path, edit.site.range.begin},
                                                    BatchEdit{.edit = edit, .plan = i});
            if(inserted) {
                continue;
            }
            auto& earlier = result.plans[it->second.plan].new_name;
            if(earlier != result.plans[i].new_name) {
                result.conflicts.push_back(
                    std::format("the name at {} would become both `{}` and `{}`",
                                where(edit.site),
                                earlier,
                                result.plans[i].new_name));
            }
            it->second.edit.heuristic &= edit.heuristic;
        }
    }
    for(auto& edit: views::values(edits)) {
        result.edits.push_back(std::move(edit));
    }
    return result;
}

std::optional<std::string> apply_renames(llvm::StringRef text, const BatchPlan& batch, Fid file) {
    std::string result;
    std::uint32_t copied = 0;
    for(auto& [edit, owner]: batch.edits) {
        if(edit.site.file != file) {
            continue;
        }
        auto& plan = batch.plans[owner];
        auto begin = edit.site.range.begin;
        auto end = begin + static_cast<std::uint32_t>(plan.old_name.size());
        if(spellings(text, begin, end, plan.old_name).empty()) {
            return std::nullopt;
        }
        result += text.slice(copied, begin);
        result += plan.new_name;
        copied = end;
    }
    result += text.substr(copied);
    return result;
}

std::optional<std::string> rename_refusal(llvm::StringRef root) {
    if(root.empty()) {
        return "a rename edits the sources of a workspace folder; open one";
    }
    return std::nullopt;
}

std::string refusal(const RenamePlan& plan) {
    auto reasons = plan.conflicts;
    if(!plan.stale.empty()) {
        reasons.push_back(std::format("these files changed since they were indexed: {}",
                                      llvm::join(plan.stale, ", ")));
    }
    return std::format("cannot rename `{}`: {}", plan.old_name, llvm::join(reasons, "; "));
}

/// `title` and up to a handful of `items`, one a line.
static void list_notice(std::string& notice,
                        std::string_view title,
                        llvm::ArrayRef<std::string> items) {
    constexpr std::size_t shown_items = 5;
    if(items.empty()) {
        return;
    }
    notice += std::format("{}{}:", notice.empty() ? "" : "\n", title);
    for(auto& item: items.take_front(shown_items)) {
        notice += std::format("\n  {}", item);
    }
    if(items.size() > shown_items) {
        notice += std::format("\n  and {} more", items.size() - shown_items);
    }
}

Renamed to_protocol(const RenamePlan& plan,
                    llvm::StringRef new_name,
                    llvm::function_ref<std::optional<int>(Fid)> version_of,
                    PositionEncoding encoding) {
    Renamed renamed;
    using DocumentChange = protocol::variant<protocol::TextDocumentEdit,
                                             protocol::CreateFile,
                                             protocol::RenameFile,
                                             protocol::DeleteFile>;
    std::vector<DocumentChange> changes;
    protocol::TextDocumentEdit* change = nullptr;
    std::vector<std::string> heuristic;
    for(auto& edit: plan.edits) {
        if(!change || change->text_document.uri != to_uri(edit.site.path)) {
            protocol::TextDocumentEdit next{
                .text_document = {.uri = to_uri(edit.site.path),
                                  .version = version_of(edit.site.file)},
            };
            change = &std::get<protocol::TextDocumentEdit>(changes.emplace_back(std::move(next)));
        }
        change->edits.emplace_back(protocol::TextEdit{
            .range = to_range(edit.site, encoding),
            .new_text = new_name.str(),
        });
        if(edit.heuristic) {
            heuristic.push_back(std::format("{}:{}", edit.site.path, edit.site.begin.line + 1));
        }
    }
    renamed.edit.document_changes = std::move(changes);

    list_notice(renamed.notice, std::format("Renaming `{}`", plan.old_name), plan.warnings);
    std::vector<std::string> left;
    for(auto& note: plan.unconfirmed) {
        left.push_back(std::format("{}:{}: {} ({})",
                                   note.site.path,
                                   note.site.begin.line + 1,
                                   note.line,
                                   note.reason));
    }
    list_notice(renamed.notice, std::format("Spellings of `{}` left alone", plan.old_name), left);
    list_notice(renamed.notice, "Renamed through a heuristic resolution", heuristic);
    return renamed;
}

}  // namespace clice::feature

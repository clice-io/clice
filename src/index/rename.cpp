#include "index/rename.h"

#include <algorithm>
#include <format>
#include <map>
#include <ranges>
#include <set>
#include <tuple>

#include "index/symbol_query.h"
#include "syntax/lexer.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/StringSet.h"
#include "clang/Basic/CharInfo.h"
#include "clang/Basic/IdentifierTable.h"
#include "clang/Basic/LangStandard.h"

namespace clice::index {

namespace {

bool identifier_char(char c) {
    return clang::isAsciiIdentifierContinue(c) || static_cast<unsigned char>(c) >= 0x80;
}

/// The offsets in `text[begin, end)` where `name` stands as a whole
/// identifier.
llvm::SmallVector<std::uint32_t, 1>
    spellings(llvm::StringRef text, std::uint32_t begin, std::uint32_t end, llvm::StringRef name) {
    llvm::SmallVector<std::uint32_t, 1> found;
    for(auto at = text.find(name, begin); at != llvm::StringRef::npos && at + name.size() <= end;
        at = text.find(name, at + 1)) {
        auto after = at + name.size();
        if((at == 0 || !identifier_char(text[at - 1])) &&
           (after == text.size() || !identifier_char(text[after]))) {
            found.push_back(static_cast<std::uint32_t>(at));
        }
    }
    return found;
}

/// Positions in one text, for the sites of tokens no row spans.
struct Lines {
    llvm::StringRef text;
    std::vector<std::uint32_t> starts;

    explicit Lines(llvm::StringRef text) :
        text(text), starts(kota::ipc::lsp::line_starts({text.data(), text.size()})) {}

    /// A token's position: never inside a newline.
    LineColumn position(std::uint32_t offset) const {
        return *Coordinates(text, starts).position(offset);
    }

    std::string line_of(std::uint32_t offset) const {
        auto line = kota::ipc::lsp::line_of(starts, offset);
        auto end = line + 1 < starts.size() ? starts[line + 1] : text.size();
        return text.slice(starts[line], end).trim().str();
    }

    Site site(Fid file, std::string path, std::uint32_t offset, std::uint32_t length) const {
        return {
            .file = file,
            .path = std::move(path),
            .range = {offset, offset + length},
            .begin = position(offset),
            .end = position(offset + length)
        };
    }
};

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

std::string where(const Site& site) {
    return std::format("{}:{}", site.path, site.begin.line + 1);
}

/// Every symbol named `name` exactly, however many.
std::vector<IndexQuery::Located> all_named(const IndexQuery& query, llvm::StringRef name) {
    SymbolQuery exact{.mode = SymbolQuery::Mode::Exact, .pattern = name.str()};
    for(std::size_t limit = 256;; limit *= 4) {
        auto hits = query.search(exact, limit);
        if(hits.size() < limit) {
            return hits;
        }
    }
}

/// The newest dialect of each language; read-only once built, so sweeps
/// on other threads may share them.
const clang::LangOptions& cxx() {
    const static auto options = raw_dialect(clang::Language::CXX, clang::LangStandard::lang_cxx26);
    return options;
}

const clang::LangOptions& c() {
    const static auto options = raw_dialect(clang::Language::C, clang::LangStandard::lang_c23);
    return options;
}

/// The class template a deduction guide deduces: the one template in the
/// guide's scope of the name its declaration spells (the symbol is listed
/// under a label).
std::optional<IndexQuery::Located> guided_template(const IndexQuery& query,
                                                   const IndexQuery::Located& guide) {
    auto text = query.serving_text(guide.site.file);
    if(!text) {
        return std::nullopt;
    }
    auto spelled = llvm::StringRef(*text).slice(guide.site.range.begin, guide.site.range.end);
    for(auto& hit: all_named(query, spelled)) {
        if(class_like(hit.symbol.kind) && hit.symbol.parent == guide.symbol.parent &&
           has_flag(hit.symbol.flags, SymbolFlags::Template)) {
            return hit;
        }
    }
    return std::nullopt;
}

}  // namespace

std::optional<std::string> invalid_identifier(llvm::StringRef name) {
    std::string text = name.str();
    Lexer lexer(text, {.lang_opts = &cxx()});
    auto token = lexer.advance();
    if(!token.is_identifier() || token.range.begin != 0 || token.range.end != text.size() ||
       !lexer.advance().is_eof()) {
        return std::format("`{}` is not an identifier", std::string_view(name));
    }
    for(auto* options: {&c(), &cxx()}) {
        clang::IdentifierTable table(*options);
        if(table.get(name).getTokenID() != clang::tok::identifier) {
            return std::format("`{}` is a keyword", std::string_view(name));
        }
    }
    return std::nullopt;
}

std::optional<SweptText> sweep_text(std::string text,
                                    llvm::StringRef old_name,
                                    llvm::StringRef new_name) {
    auto size = static_cast<std::uint32_t>(text.size());
    if(spellings(text, 0, size, old_name).empty() && spellings(text, 0, size, new_name).empty()) {
        return std::nullopt;
    }
    SweptText swept;
    bool directive = false;
    Lexer lexer(text, {.lang_opts = &cxx()});
    for(auto token = lexer.advance(); !token.is_eof(); token = lexer.advance()) {
        if(token.is_directive_hash()) {
            directive = true;
        } else if(token.is_eod()) {
            directive = false;
        }
        if(!token.is_identifier()) {
            continue;
        }
        auto spelled = token.text(text);
        if(spelled == old_name) {
            swept.old_tokens.push_back({.offset = token.range.begin, .directive = directive});
        } else if(spelled == new_name) {
            swept.spells_new = true;
        }
    }
    if(swept.old_tokens.empty() && !swept.spells_new) {
        return std::nullopt;
    }
    swept.text = std::move(text);
    return swept;
}

std::expected<RenameTarget, std::string> rename_target(const IndexQuery& query,
                                                       const IndexQuery::Located& named) {
    auto& symbol = named.symbol;
    if(symbol.kind == SymbolKind::Macro) {
        return std::unexpected("renaming a macro is not supported yet");
    }
    if(!renamable(symbol.kind)) {
        return std::unexpected(std::format("`{}` cannot be renamed", symbol.name));
    }
    if(has_flag(symbol.flags, SymbolFlags::Unnamed)) {
        return std::unexpected(std::format("{} has no name to rename", symbol.name));
    }

    auto root = named;
    switch(name_form(symbol.flags)) {
        case NameForm::Identifier: break;
        case NameForm::Constructor:
        case NameForm::Destructor: {
            auto owner = query.resolve(symbol.parent, named.site.file);
            if(!owner) {
                return std::unexpected(
                    std::format("the class of `{}` is not indexed", symbol.name));
            }
            root = std::move(*owner);
            break;
        }
        case NameForm::Other: {
            auto deduced = guided_template(query, named);
            if(!deduced) {
                return std::unexpected(std::format("`{}` cannot be renamed", symbol.name));
            }
            root = std::move(*deduced);
            break;
        }
        case NameForm::Conversion:
        case NameForm::Operator:
        case NameForm::Literal: {
            return std::unexpected(
                std::format("`{}` is named by the language, not by an identifier", symbol.name));
        }
    }
    if(has_flag(root.symbol.flags, SymbolFlags::Specialization)) {
        auto primaries =
            query.located_targets(root.symbol.hash, root.site.file, RelationKind::Primary);
        if(!primaries.empty()) {
            root = std::move(primaries.front());
        }
    }
    if(has_flag(root.symbol.flags, SymbolFlags::SystemHeader)) {
        return std::unexpected(
            std::format("`{}` is declared in a system header", root.symbol.display_name()));
    }
    if(has_flag(root.symbol.flags, SymbolFlags::SpelledInMacro)) {
        return std::unexpected(std::format("every declaration of `{}` is spelled by a macro",
                                           root.symbol.display_name()));
    }

    RenameTarget target{.symbol = root};
    llvm::DenseSet<SymbolHash> seen;
    std::vector<IndexQuery::Located> pending{root};
    while(!pending.empty()) {
        auto next = std::move(pending.back());
        pending.pop_back();
        if(!seen.insert(next.symbol.hash).second) {
            continue;
        }
        auto follow = [&](RelationKind kind) {
            llvm::append_range(pending,
                               query.located_targets(next.symbol.hash, next.site.file, kind));
        };
        follow(RelationKind::Specialization);
        if(class_like(next.symbol.kind)) {
            follow(RelationKind::Constructor);
            follow(RelationKind::Destructor);
        } else if(next.symbol.kind == SymbolKind::Method) {
            follow(RelationKind::Interface);
            follow(RelationKind::Implementation);
        }
        target.group.push_back(std::move(next));
    }
    return target;
}

std::expected<CursorRename, std::string> rename_at(const IndexQuery& query,
                                                   const IndexQuery::Cursor& cursor) {
    std::optional<RenameTarget> found;
    std::string refused;
    for(auto& named: query.resolve_at(cursor)) {
        auto target = rename_target(query, named);
        if(!target) {
            refused = std::move(target.error());
            continue;
        }
        if(found && found->symbol.symbol.hash != target->symbol.symbol.hash) {
            return std::unexpected(
                "the name here resolves to several symbols; rename from one of their declarations");
        }
        found = std::move(*target);
    }
    if(!found) {
        return std::unexpected(refused.empty() ? "no symbol here to rename" : refused);
    }

    auto& site = cursor.site;
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
    auto token = Lines(*text).site(site.file, site.path, offsets.front(), name.size());
    return CursorRename{.target = std::move(*found), .token = std::move(token)};
}

RenamePlan plan_rename(const IndexQuery& query,
                       FileTable& files,
                       const RenameTarget& target,
                       llvm::StringRef new_name,
                       const RenameScope& scope) {
    auto& root = target.symbol.symbol;
    RenamePlan plan{.old_name = root.name};
    llvm::StringRef old_name = plan.old_name;
    if(new_name == old_name) {
        return plan;
    }
    if(auto invalid = invalid_identifier(new_name)) {
        plan.conflicts.push_back(std::move(*invalid));
        return plan;
    }
    llvm::DenseSet<SymbolHash> group;
    for(auto& member: target.group) {
        group.insert(member.symbol.hash);
    }
    // A deduction guide relates to nothing and has no definition to find
    // it by: the sweep meets each one where it spells the template's name.
    bool guided = class_like(root.kind) && has_flag(root.flags, SymbolFlags::Template);
    auto renames = [&](SymbolHash symbol, Fid file) {
        if(group.contains(symbol)) {
            return true;
        }
        if(!guided) {
            return false;
        }
        auto info = query.symbol_info(symbol, file);
        return info && name_form(info->flags) == NameForm::Other && info->parent == root.parent;
    };

    // The text every row of a file indexes, and its positions.
    struct Text {
        std::optional<std::string> text;
        std::optional<Lines> lines;
    };

    llvm::DenseMap<Fid, std::unique_ptr<Text>> texts;
    auto text_of = [&](Fid file) -> Text& {
        auto& slot = texts[file];
        if(!slot) {
            slot = std::make_unique<Text>();
            slot->text = query.serving_text(file);
            if(slot->text) {
                slot->lines.emplace(*slot->text);
            }
        }
        return *slot;
    };

    // Another renamable symbol of the old name the token names too: a
    // variant of a shared header compiled under other definitions, or an
    // overload a dependent call leaves open. Changing the token changes
    // its use as well.
    auto shared_with = [&](Fid file, std::uint32_t offset) -> std::optional<std::string> {
        auto cursor = query.symbol_at(file, offset);
        if(!cursor) {
            return std::nullopt;
        }
        for(auto symbol: cursor->symbols) {
            if(renames(symbol, file)) {
                continue;
            }
            if(auto info = query.symbol_info(symbol, file);
               info && info->name == old_name && renamable(info->kind)) {
                return query.qualified_name(symbol);
            }
        }
        return std::nullopt;
    };

    llvm::StringSet<> stale;
    std::map<std::pair<Fid, std::uint32_t>, RenameEdit> edits;
    std::set<std::pair<Fid, std::uint32_t>> held;
    auto add_edit = [&](Fid file, const Site& near, std::uint32_t offset, bool heuristic) {
        auto& text = text_of(file);
        if(auto other = shared_with(file, offset)) {
            if(held.insert({file, offset}).second) {
                plan.unconfirmed.push_back({
                    .site = text.lines->site(file, near.path, offset, old_name.size()),
                    .reason = std::format("the name here also refers to `{}`, under another "
                                          "build configuration or as another candidate of a "
                                          "dependent call",
                                          *other),
                    .line = text.lines->line_of(offset),
                });
            }
            return;
        }
        auto [it, inserted] = edits.try_emplace(
            {file, offset},
            RenameEdit{.site = text.lines->site(file, near.path, offset, old_name.size()),
                       .heuristic = heuristic});
        if(!inserted) {
            it->second.heuristic &= heuristic;
        }
    };

    for(auto& member: target.group) {
        for(auto kind: {RelationKind::Definition,
                        RelationKind::Declaration,
                        RelationKind::Reference,
                        RelationKind::WeakReference}) {
            for(auto& site: query.sites(member.symbol.hash, member.site.file, kind)) {
                auto& text = text_of(site.file);
                if(!text.text) {
                    stale.insert(site.path);
                    continue;
                }
                auto offsets = spellings(*text.text, site.range.begin, site.range.end, old_name);
                if(offsets.empty()) {
                    // A row on punctuation (a construction's paren) spells
                    // no name; one on another name is a macro's invocation.
                    auto written =
                        llvm::StringRef(*text.text).slice(site.range.begin, site.range.end);
                    if(llvm::any_of(written, identifier_char)) {
                        plan.unconfirmed.push_back(
                            {.site = site,
                             .reason = std::format("the name is written here as `{}`",
                                                   std::string_view(written)),
                             .line = text.lines->line_of(site.range.begin)});
                    }
                    continue;
                }
                for(auto offset: offsets) {
                    add_edit(site.file, site, offset, kind == RelationKind::WeakReference);
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
        auto path = files.resolve(file).str();
        if(!scope.editable(path)) {
            plan.conflicts.push_back(
                std::format("the rename would change {}, which is not a workspace source "
                            "(a system header, a dependency or a generated file)",
                            edit.site.path));
            continue;
        }
        // The edits are offsets into the text the rows index; the sweep
        // below compares only the files that still spell a name.
        auto current = scope.read(path);
        if(!current || current->text != *text_of(file).text) {
            stale.insert(edit.site.path);
        }
    }

    // Tokens spelling the old name that no edit covers — another symbol's,
    // or names the index cannot see — and the files whose collisions with
    // the new name the index must see current.
    for(auto& path: scope.files) {
        auto current = scope.read(path);
        if(!current) {
            continue;
        }
        auto file = files.intern(Spelling::absolute(path));
        auto display = files.display(file);
        auto& served = text_of(file);
        bool indexed = served.text.has_value();
        if(indexed ? *served.text != current->text : query.indexes(file) || scope.units_pending) {
            stale.insert(display);
            continue;
        }
        Lines lines(current->text);
        for(auto [offset, directive]: current->old_tokens) {
            if(!indexed) {
                plan.unconfirmed.push_back(
                    {.site = lines.site(file, display, offset, old_name.size()),
                     .reason = "the index holds no rows of this file: no unit it indexes "
                               "compiles or includes it",
                     .line = lines.line_of(offset)});
                continue;
            }
            if(edits.contains({file, offset})) {
                continue;
            }
            if(auto cursor = query.symbol_at(file, offset);
               cursor && cursor->site.range.begin <= offset && offset < cursor->site.range.end) {
                if(llvm::any_of(cursor->symbols,
                                [&](SymbolHash symbol) { return renames(symbol, file); })) {
                    add_edit(file, cursor->site, offset, false);
                }
                continue;
            }
            plan.unconfirmed.push_back(
                {.site = lines.site(file, display, offset, old_name.size()),
                 .reason = directive ? "inside a preprocessor directive, which the index does not "
                                       "resolve"
                                     : "the index ties no symbol to it: a dependent name it "
                                       "could not resolve, an inactive #if branch, or code no "
                                       "unit compiles",
                 .line = lines.line_of(offset)});
        }
    }

    for(auto& key: llvm::make_first_range(edits)) {
        edited.insert(key.first);
    }

    // What the new name would collide with: a macro anywhere, a
    // declaration in the same scope, a member of a class above or below.
    llvm::SmallVector<IndexQuery::Located> named;
    llvm::DenseSet<SymbolHash> listed;
    for(auto& hit: all_named(query, new_name)) {
        if(listed.insert(hit.symbol.hash).second) {
            named.push_back(std::move(hit));
        }
    }
    for(auto file: edited) {
        auto source = query.serving(file);
        if(!source) {
            continue;
        }
        source->rows->for_each_relation([&](SymbolHash hash, const Relation&) {
            if(!listed.insert(hash).second) {
                return true;
            }
            if(auto info = query.symbol_info(hash, file); info && info->name == new_name) {
                if(auto located = query.resolve(hash, file)) {
                    named.push_back(std::move(*located));
                }
            }
            return true;
        });
    }

    // The scope a name is looked up in: an unscoped enumerator's is its
    // enum's.
    auto scope_of = [&](const SymbolRef& symbol, Fid anchor) {
        if(symbol.kind == SymbolKind::EnumMember &&
           has_flag(symbol.flags, SymbolFlags::Completable)) {
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
    llvm::DenseSet<SymbolHash> hierarchy;
    auto parent = query.symbol_info(home, anchor);
    if(parent && class_like(parent->kind)) {
        if(auto located = query.resolve(home, anchor)) {
            hierarchy.insert(located->symbol.hash);
            for(bool up: {true, false}) {
                std::vector<IndexQuery::Located> pending{*located};
                while(!pending.empty()) {
                    auto next = std::move(pending.back());
                    pending.pop_back();
                    auto types = query.type_hierarchy(next.symbol.hash, next.site.file, {});
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
    // Both languages reserve `__x` and `_X` everywhere, and `_x` at file
    // and global namespace scope.
    if(new_name.contains("__") ||
       (new_name.starts_with("_") &&
        (home == 0 || (new_name.size() > 1 && clang::isUppercase(new_name[1]))))) {
        plan.warnings.push_back(
            std::format("`{}` is reserved for the implementation", std::string_view(new_name)));
    }
    if(parent && class_like(parent->kind) && parent->name == new_name) {
        plan.conflicts.push_back(
            std::format("`{}` names the class the member belongs to", std::string_view(new_name)));
    }

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
            if(function_like(symbol.kind) && function_like(root.kind)) {
                plan.warnings.push_back(
                    std::format("`{}` already names a function in the same scope ({}): the "
                                "renamed function overloads it, which fails to build in C or "
                                "when their parameters are the same",
                                std::string_view(new_name),
                                where(other.site)));
            } else if(local && symbol.kind == root.kind &&
                      (root.kind == SymbolKind::Parameter || root.kind == SymbolKind::Label)) {
                plan.conflicts.push_back(
                    std::format("`{}` already names a {} of the same function ({})",
                                std::string_view(new_name),
                                root.kind == SymbolKind::Label ? "label" : "parameter",
                                where(other.site)));
            } else if(local) {
                plan.warnings.push_back(
                    std::format("`{}` already names a local of the same function ({}): declared "
                                "in the same block, the two fail to build; in nested blocks, "
                                "the inner hides the outer",
                                std::string_view(new_name),
                                where(other.site)));
            } else if(symbol.kind == SymbolKind::Namespace && root.kind == SymbolKind::Namespace) {
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
            plan.conflicts.push_back(
                std::format("`{}` is a member of the renamed class ({}), which may not share its "
                            "name",
                            std::string_view(new_name),
                            where(other.site)));
            continue;
        }
        // A local of a function holding an edited use may capture it.
        if(edited.contains(other.site.file)) {
            auto owner = query.symbol_info(other_home, other.site.file);
            auto function = owner && function_like(owner->kind)
                                ? query.resolve(other_home, other.site.file)
                                : std::nullopt;
            if(function) {
                auto& body = function->extent;
                if(llvm::any_of(llvm::make_first_range(edits), [&](const auto& key) {
                       return key.first == body.file && body.range.begin <= key.second &&
                              key.second < body.range.end;
                   })) {
                    plan.warnings.push_back(
                        std::format("`{}` is declared in {} ({}): where it is in scope, it "
                                    "captures the renamed name used there",
                                    std::string_view(new_name),
                                    query.qualified_name(other_home),
                                    where(other.site)));
                }
                continue;
            }
        }
        if(hierarchy.contains(other_home)) {
            plan.conflicts.push_back(
                std::format("`{}` is a member of {} ({}), which the renamed member would hide or "
                            "be hidden by",
                            std::string_view(new_name),
                            query.qualified_name(other_home),
                            where(other.site)));
        }
    }

    for(auto& [key, edit]: edits) {
        plan.edits.push_back(std::move(edit));
    }
    std::ranges::sort(plan.edits, [](const RenameEdit& lhs, const RenameEdit& rhs) {
        return std::tie(lhs.site.path, lhs.site.range.begin) <
               std::tie(rhs.site.path, rhs.site.range.begin);
    });
    std::ranges::sort(plan.unconfirmed, [](const RenameNote& lhs, const RenameNote& rhs) {
        return std::tie(lhs.site.path, lhs.site.range.begin) <
               std::tie(rhs.site.path, rhs.site.range.begin);
    });
    for(auto& entry: stale) {
        plan.stale.push_back(entry.getKey().str());
    }
    std::ranges::sort(plan.stale);
    return plan;
}

std::optional<std::string>
    apply_rename(llvm::StringRef text, const RenamePlan& plan, Fid file, llvm::StringRef new_name) {
    std::string result;
    std::uint32_t copied = 0;
    for(auto& edit: plan.edits) {
        if(edit.site.file != file) {
            continue;
        }
        auto begin = edit.site.range.begin;
        auto end = begin + static_cast<std::uint32_t>(plan.old_name.size());
        if(spellings(text, begin, end, plan.old_name).empty()) {
            return std::nullopt;
        }
        result += text.slice(copied, begin);
        result += new_name;
        copied = end;
    }
    result += text.substr(copied);
    return result;
}

}  // namespace clice::index

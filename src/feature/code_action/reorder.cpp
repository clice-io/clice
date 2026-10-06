module;

#include "modules/prelude.h"

module clice;

import :compile.compilation_unit;
import :compile.semantics;
import :feature.code_action.action;

namespace clice::feature::action {

namespace {

/// One definition's lines in the main file: the comment block directly
/// above it through the end of its last line.
struct Slot {
    LocalSourceRange range;
    /// Its position in declaration order.
    std::uint32_t rank;
    /// The namespace block it is written in: definitions only move
    /// within their block, whose scope their names are spelled for.
    const clang::DeclContext* block;
    /// The innermost conditional branch enclosing it (the offset of its
    /// `#if`, `#elif` or `#else`; none at top level): definitions only
    /// move within their branch, or a symbol would come and go with a
    /// macro it never depended on.
    std::optional<std::uint32_t> branch;
};

/// The innermost conditional branch open at `offset` of the main file.
/// The lexical block directives rather than the directive table: the
/// preamble's conditionals are compiled into the PCH and never reach it.
std::optional<std::uint32_t> enclosing_branch(CompilationUnitRef unit, std::uint32_t offset) {
    llvm::SmallVector<std::uint32_t, 4> open;
    for(const auto& directive: unit.semantics().block_directives()) {
        auto at = directive.range.begin;
        if(at >= offset) {
            break;
        }
        using enum LexicalInfo::BlockDirective::Kind;
        switch(directive.kind) {
            case If: open.push_back(at); break;
            case Else:
                if(!open.empty()) {
                    open.back() = at;
                }
                break;
            case EndIf:
                if(!open.empty()) {
                    open.pop_back();
                }
                break;
            case Region:
            case EndRegion: break;
        }
    }
    if(open.empty()) {
        return std::nullopt;
    }
    return open.back();
}

/// The lines the definition owns; nullopt when it spans a macro, another
/// file or a conditional directive, whose text cannot be moved as a
/// block.
std::optional<LocalSourceRange> definition_lines(CompilationUnitRef unit,
                                                 const clang::FunctionDecl* definition) {
    auto range = main_range(unit, written_declaration(definition)->getSourceRange());
    if(!range) {
        return std::nullopt;
    }
    auto content = unit.main_content();
    auto main = unit.main_file();
    auto begin = range->begin;

    // Attribute specifiers right before the declaration (`[[deprecated]]`
    // on a line of its own, or a macro spelling one) lie outside its
    // range.
    auto tokens = unit.spelled_tokens(main);
    auto& SM = unit.context().getSourceManager();
    auto token_at = [&](std::uint32_t offset) {
        return std::ranges::partition_point(tokens, [&](const clang::syntax::Token& token) {
            return unit.file_offset(token.location()) < offset;
        });
    };
    auto first = token_at(begin);
    while(first != tokens.begin()) {
        auto previous = std::prev(first);
        if(previous != tokens.begin() && previous->kind() == clang::tok::r_square &&
           std::prev(previous)->kind() == clang::tok::r_square) {
            std::uint32_t depth = 0;
            auto open = previous;
            for(;; --open) {
                if(open->kind() == clang::tok::r_square) {
                    depth += 1;
                } else if(open->kind() == clang::tok::l_square) {
                    depth -= 1;
                }
                if(depth == 0 || open == tokens.begin()) {
                    break;
                }
            }
            if(depth != 0 || std::next(open)->kind() != clang::tok::l_square) {
                break;
            }
            first = open;
            continue;
        }
        std::optional<clang::SourceLocation> invocation;
        for(const auto* attr: definition->attrs()) {
            if(attr->getLocation().isMacroID()) {
                auto expansion = SM.getExpansionRange(attr->getLocation());
                if(expansion.getEnd() == previous->location()) {
                    invocation = expansion.getBegin();
                }
            }
        }
        if(!invocation) {
            break;
        }
        first = token_at(unit.file_offset(*invocation));
    }
    begin = unit.file_offset(first->location());

    // Comments directly above the definition travel with it: each on
    // lines of its own, separated from what follows by nothing but
    // whitespace holding a single line break. A comment trailing the
    // previous definition's line belongs to that line.
    if(const auto* comments = unit.context().Comments.getCommentsInFile(main)) {
        auto it = comments->lower_bound(begin);
        while(it != comments->begin()) {
            --it;
            auto comment_begin = it->first;
            auto comment_end = unit.file_offset(it->second->getEndLoc()) +
                               unit.token_length(it->second->getEndLoc());
            auto line = line_begin(content, comment_begin);
            auto gap = content.substr(comment_end, begin - comment_end);
            if(comment_end > begin || !gap.trim().empty() || gap.count('\n') != 1 ||
               !content.substr(line, comment_begin - line).trim().empty()) {
                break;
            }
            begin = comment_begin;
        }
    }
    auto line = line_begin(content, begin);
    if(content.substr(line, begin - line).trim().empty()) {
        begin = line;
    }
    LocalSourceRange lines{begin, line_end(content, range->end - 1)};

    using enum LexicalInfo::BlockDirective::Kind;
    if(llvm::any_of(unit.semantics().block_directives(), [&](const auto& directive) {
           return llvm::is_contained({If, Else, EndIf}, directive.kind) &&
                  directive.range.begin >= lines.begin && directive.range.begin < lines.end;
       })) {
        return std::nullopt;
    }
    return lines;
}

/// An order the text relies on, between two offsets of the main file: a
/// name's declaration and its use, a macro's definition and its
/// expansion, an expansion and the macro's `#undef`, a using-declaration
/// and the definitions after it, an include and the definitions around
/// it.
struct Dependency {
    std::uint32_t before;
    std::uint32_t after;
};

/// The declarations a use of `decl` needs ahead of it: its first
/// declaration, and its definition when the use may need what only the
/// definition says — a complete type, a deduced return type, a value for
/// constant evaluation — along with the definitions of the class types
/// it passes by value.
llvm::SmallVector<const clang::Decl*, 4> requirements(clang::ASTContext& context,
                                                      const clang::NamedDecl* decl) {
    llvm::SmallVector<const clang::Decl*, 4> out{decl->getCanonicalDecl()};
    auto add = [&](const clang::Decl* definition) {
        if(definition) {
            out.push_back(definition);
        }
    };
    auto complete = [&](clang::QualType type) {
        if(auto* tag = type->getAsTagDecl()) {
            add(tag->getDefinition());
        }
    };
    if(auto* described = llvm::dyn_cast<clang::RedeclarableTemplateDecl>(decl)) {
        decl = described->getTemplatedDecl();
    }
    if(auto* tag = llvm::dyn_cast<clang::TagDecl>(decl)) {
        add(tag->getDefinition());
    } else if(auto* function = llvm::dyn_cast<clang::FunctionDecl>(decl)) {
        if(function->isConstexpr() ||
           function->getDeclaredReturnType()->getContainedDeducedType()) {
            add(function->getDefinition());
        }
        complete(function->getReturnType());
        for(const auto* parameter: function->parameters()) {
            complete(parameter->getType());
        }
    } else if(auto* variable = llvm::dyn_cast<clang::VarDecl>(decl)) {
        if(variable->isUsableInConstantExpressions(context)) {
            add(variable->getInitializingDeclaration());
        }
        complete(variable->getType());
    }
    return out;
}

/// The orders the text between the first and the last slot relies on.
std::vector<Dependency> dependencies(CompilationUnitRef unit, llvm::ArrayRef<Slot> slots) {
    auto main = unit.main_file();
    LocalSourceRange region{slots.front().range.begin, slots.back().range.end};
    auto offset_of = [&](clang::SourceLocation location) -> std::optional<std::uint32_t> {
        location = unit.expansion_location(location);
        if(location.isInvalid() || unit.file_id(location) != main) {
            return std::nullopt;
        }
        auto offset = unit.file_offset(location);
        return offset >= region.begin && offset < region.end ? std::optional(offset) : std::nullopt;
    };
    std::vector<Dependency> out;
    auto depend = [&](clang::SourceLocation before, clang::SourceLocation after) {
        auto first = offset_of(before);
        auto second = offset_of(after);
        if(first && second && *first < *second) {
            out.push_back({*first, *second});
        }
    };

    const auto& semantics = unit.semantics();
    auto entries = semantics.node_entries();
    for(std::uint32_t i = 0; i < entries.size() && entries[i].node.is_ast();) {
        if(const auto* decl = entries[i].node.get<clang::Decl>()) {
            auto range = main_range(unit, decl->getSourceRange());
            if(range && (range->end <= region.begin || range->begin >= region.end)) {
                i = entries[i].subtree_end;
                continue;
            }
            // Lookup after a using-declaration or -directive may find
            // what it brings in.
            if(llvm::isa<clang::UsingDecl, clang::UsingDirectiveDecl, clang::UsingEnumDecl>(decl) &&
               at_file_scope(decl->getDeclContext())) {
                if(auto at = offset_of(decl->getLocation())) {
                    for(const auto& slot: slots) {
                        if(slot.range.begin > *at) {
                            out.push_back({*at, slot.range.begin});
                        }
                    }
                }
            }
        }
        for(const auto& occurrence: resolve_occurrences(semantics, i)) {
            if(occurrence.kind.isDeclOrDef()) {
                continue;
            }
            for(const auto* required: requirements(unit.context(), occurrence.decl)) {
                depend(required->getLocation(), occurrence.location);
            }
        }
        i += 1;
    }

    // An included file may rely on the definitions above it and provide
    // for those below: no definition crosses it. One inside a definition
    // moves with it.
    for(const auto& include: semantics.include_directives()) {
        if(include.begin <= region.begin || include.begin >= region.end ||
           llvm::any_of(slots, [&](const Slot& slot) {
               return include.begin >= slot.range.begin && include.begin < slot.range.end;
           })) {
            continue;
        }
        for(const auto& slot: slots) {
            if(slot.range.begin < include.begin) {
                out.push_back({slot.range.begin, include.begin});
            } else {
                out.push_back({include.begin, slot.range.begin});
            }
        }
    }

    if(auto it = unit.directives().find(main); it != unit.directives().end()) {
        llvm::DenseMap<const clang::MacroInfo*, llvm::SmallVector<clang::SourceLocation, 1>> undefs;
        for(const auto& macro: it->second.macros) {
            if(macro.kind == MacroRef::Undef) {
                undefs[macro.macro].push_back(macro.loc);
            }
        }
        for(const auto& macro: it->second.macros) {
            if(macro.kind != MacroRef::Ref) {
                continue;
            }
            depend(macro.macro->getDefinitionLoc(), macro.loc);
            if(auto undef = undefs.find(macro.macro); undef != undefs.end()) {
                for(auto location: undef->second) {
                    depend(macro.loc, location);
                }
            }
        }
    }
    return out;
}

/// The edits permuting the slots of each block and branch into rank
/// order; empty when they already are, or when two definitions share a
/// line and their slots overlap. A definition whose move would break an
/// order the text relies on stays where it is, the others ordered around
/// it.
std::vector<TextReplacement> permutation(CompilationUnitRef unit, std::vector<Slot> slots) {
    std::ranges::sort(slots, {}, [](const Slot& slot) { return slot.range.begin; });
    for(auto [previous, slot]: llvm::zip(slots, llvm::drop_begin(slots))) {
        if(slot.range.begin < previous.range.end) {
            return {};
        }
    }
    llvm::MapVector<std::pair<const clang::DeclContext*, std::optional<std::uint32_t>>,
                    llvm::SmallVector<std::size_t>>
        blocks;
    for(auto [index, slot]: llvm::enumerate(slots)) {
        blocks[{slot.block, slot.branch}].push_back(index);
    }

    // The slot whose text lands in each slot, and where each slot's text
    // lands.
    std::vector<std::size_t> source(slots.size());
    std::vector<std::size_t> target(slots.size());
    std::vector<bool> pinned(slots.size(), false);
    // Orders each block's unpinned slots by rank; whether any text moves.
    auto assign = [&] {
        for(const auto& members: llvm::make_second_range(blocks)) {
            llvm::SmallVector<std::size_t> movable;
            for(auto index: members) {
                if(pinned[index]) {
                    source[index] = index;
                } else {
                    movable.push_back(index);
                }
            }
            auto ordered = movable;
            std::ranges::sort(ordered, {}, [&](std::size_t index) { return slots[index].rank; });
            for(auto [position, text]: llvm::zip(movable, ordered)) {
                source[position] = text;
            }
        }
        bool moves = false;
        for(auto [position, text]: llvm::enumerate(source)) {
            target[text] = position;
            moves |= position != text;
        }
        return moves;
    };
    auto slot_of = [&](std::uint32_t offset) -> std::optional<std::size_t> {
        auto it = std::ranges::upper_bound(slots, offset, {}, [](const Slot& slot) {
            return slot.range.begin;
        });
        if(it == slots.begin() || offset >= std::prev(it)->range.end) {
            return std::nullopt;
        }
        return static_cast<std::size_t>(std::prev(it) - slots.begin());
    };
    // The order of an offset's text after the permutation.
    auto landing = [&](std::uint32_t offset) -> std::pair<std::uint32_t, std::uint32_t> {
        if(auto slot = slot_of(offset)) {
            return std::pair(slots[target[*slot]].range.begin, offset - slots[*slot].range.begin);
        }
        return {offset, 0};
    };

    if(!assign()) {
        return {};
    }
    auto orders = dependencies(unit, slots);
    // A broken order has an end in a moved slot: pinning it terminates.
    while(true) {
        bool broken = false;
        for(const auto& order: orders) {
            if(landing(order.before) > landing(order.after)) {
                for(auto offset: {order.before, order.after}) {
                    if(auto slot = slot_of(offset)) {
                        pinned[*slot] = true;
                    }
                }
                broken = true;
            }
        }
        if(!broken) {
            break;
        }
        assign();
    }

    auto content = unit.main_content();
    std::vector<TextReplacement> edits;
    for(auto [position, text]: llvm::enumerate(source)) {
        if(position == text) {
            continue;
        }
        auto range = slots[text].range;
        auto moved = content.substr(range.begin, range.length()).str();
        // The file's last line may lack its newline: text moved off it
        // must not run into the next definition.
        if(!moved.ends_with('\n')) {
            moved += '\n';
        }
        edits.push_back({slots[position].range, std::move(moved)});
    }
    return edits;
}

}  // namespace

void reorder_definitions(const Context& ctx, std::vector<CodeAction>& out) {
    auto unit = ctx.unit;
    const auto* record = ctx.node.get<clang::CXXRecordDecl>();
    const clang::FunctionDecl* anchor = nullptr;
    if(!record) {
        anchor = ctx.node.get<clang::FunctionDecl>();
        if(!anchor || !anchor->isThisDeclarationADefinition() ||
           !at_file_scope(anchor->getLexicalDeclContext())) {
            return;
        }
        if(auto* method = llvm::dyn_cast<clang::CXXMethodDecl>(anchor)) {
            if(!method->isOutOfLine()) {
                return;
            }
            record = method->getParent();
        }
    }

    std::vector<Slot> slots;
    std::string title;
    if(record) {
        record = record->getDefinition();
        if(!record) {
            return;
        }
        llvm::DenseMap<const clang::Decl*, std::uint32_t> ranks;
        for(const auto* member: record->decls()) {
            if(auto* described = llvm::dyn_cast<clang::FunctionTemplateDecl>(member)) {
                member = described->getTemplatedDecl();
            }
            if(llvm::isa<clang::FunctionDecl>(member)) {
                ranks.try_emplace(member->getCanonicalDecl(), ranks.size());
            }
        }
        for(const auto* definition: out_of_line_definitions(unit, record)) {
            auto rank = ranks.find(definition->getCanonicalDecl());
            if(rank == ranks.end()) {
                continue;
            }
            auto lines = definition_lines(unit, definition);
            if(!lines) {
                return;
            }
            slots.push_back({*lines,
                             rank->second,
                             definition->getLexicalDeclContext(),
                             enclosing_branch(unit, lines->begin)});
        }
        title = std::format("Reorder definitions of '{}' by declaration order", record->getName());
    } else {
        // Free functions: the definitions in this file of functions
        // declared in the file the anchor's first declaration lives in,
        // ordered as that file declares them.
        const auto* first = anchor->getCanonicalDecl();
        if(first == anchor || !first->getLocation().isFileID()) {
            return;
        }
        auto declaring = unit.file_id(first->getLocation());
        for_each_file_scope_decl(unit, [&](const clang::Decl* decl) {
            if(auto* described = llvm::dyn_cast<clang::FunctionTemplateDecl>(decl)) {
                decl = described->getTemplatedDecl();
            }
            auto* function = llvm::dyn_cast<clang::FunctionDecl>(decl);
            if(!function || !function->isThisDeclarationADefinition() ||
               llvm::isa<clang::CXXMethodDecl>(function)) {
                return;
            }
            const auto* canonical = function->getCanonicalDecl();
            if(canonical == function || !canonical->getLocation().isFileID() ||
               unit.file_id(canonical->getLocation()) != declaring) {
                return;
            }
            if(auto lines = definition_lines(unit, function)) {
                slots.push_back({*lines,
                                 unit.file_offset(canonical->getLocation()),
                                 function->getLexicalDeclContext(),
                                 enclosing_branch(unit, lines->begin)});
            }
        });
        title = "Reorder definitions by declaration order";
    }

    if(slots.size() < 2) {
        return;
    }
    auto edits = permutation(unit, std::move(slots));
    if(edits.empty()) {
        return;
    }
    out.push_back(CodeAction{
        .title = std::move(title),
        .kind = protocol::CodeActionKind::RefactorRewrite,
        .edits = std::move(edits),
    });
}

}  // namespace clice::feature::action

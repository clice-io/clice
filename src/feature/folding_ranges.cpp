#include <algorithm>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "compile/compilation_unit.h"
#include "compile/semantics.h"
#include "feature/feature.h"
#include "semantic/decls.h"
#include "support/text.h"

#include "llvm/Support/Casting.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/AST/Expr.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/StmtCXX.h"

namespace clice::feature {

namespace {

/// Collects folding ranges by walking the unit's cached Semantics node table —
/// the DFS pre-order record of the main file's written AST — instead of
/// running another RecursiveASTVisitor over the TU. Folding needs no nesting
/// state: every recorded decl and stmt contributes its ranges independently.
///
/// Fold kinds are plain strings on the wire (LSP standardizes only `comment`,
/// `imports` and `region`; servers may add custom values).
///
/// A delimited fold spans its delimiters, which `collapsed_text` repeats; a
/// section fold (an access-specifier section, a conditional branch, a
/// region, a module fragment) runs from the end of its header line to the
/// next header, which stays visible; a run of line comments, include
/// directives or using declarations on consecutive lines folds below its
/// first line.
class FoldingRangeCollector {
public:
    explicit FoldingRangeCollector(CompilationUnitRef unit) :
        unit(unit), content(unit.main_content()) {
        for(auto& directive: unit.semantics().block_directives()) {
            block_directives.push_back(directive.range.begin);
        }
    }

    auto collect() -> std::vector<FoldingRange> {
        auto nodes = unit.semantics().node_entries();
        std::uint32_t index = 0;
        while(index < nodes.size()) {
            const Semantics::Node& entry = nodes[index];
            if(!entry.node.is_ast()) {
                // The preprocessor segment follows the AST segment; directive
                // folds are collected from the lexical scan below.
                break;
            }

            if(entry.node.kind() == SemanticNode::Kind::Decl) {
                const auto* decl = entry.node.get<clang::Decl>();
                if(decls::is_instantiation(decl)) {
                    index = entry.subtree_end;
                    continue;
                }
                collect_decl(decl);
            } else if(entry.node.kind() == SemanticNode::Kind::Stmt) {
                collect_stmt(entry.node.get<clang::Stmt>(), entry.parent);
            }

            index += 1;
        }

        collect_block_directives(unit.semantics().block_directives());
        collect_module_fragments(unit.semantics().module_declarations());
        collect_comments(unit.semantics().comments());
        add_runs(unit.semantics().include_directives(), protocol::FoldingRangeKind::Imports);
        collect_raw_strings(unit.semantics().raw_strings());
        add_runs(usings, "usingDeclaration");

        // Order by kind and text after position so equal entries are adjacent
        // and the output stays deterministic under the unstable sort.
        std::ranges::sort(ranges, [](const FoldingRange& lhs, const FoldingRange& rhs) {
            if(lhs.range.begin != rhs.range.begin) {
                return lhs.range.begin < rhs.range.begin;
            }
            if(lhs.range.end != rhs.range.end) {
                return lhs.range.end < rhs.range.end;
            }
            if(lhs.kind != rhs.kind) {
                return lhs.kind < rhs.kind;
            }
            return lhs.collapsed_text < rhs.collapsed_text;
        });

        auto duplicates =
            std::ranges::unique(ranges, [](const FoldingRange& lhs, const FoldingRange& rhs) {
                return lhs.range.begin == rhs.range.begin && lhs.range.end == rhs.range.end &&
                       lhs.kind == rhs.kind && lhs.collapsed_text == rhs.collapsed_text;
            });
        ranges.erase(duplicates.begin(), duplicates.end());

        return std::move(ranges);
    }

private:
    void collect_decl(const clang::Decl* decl) {
        collect_template_parameters(decl);

        if(llvm::isa<clang::UsingDecl,
                     clang::UsingDirectiveDecl,
                     clang::UsingEnumDecl,
                     clang::UnresolvedUsingValueDecl,
                     clang::UnresolvedUsingTypenameDecl>(decl)) {
            // A declaration a macro produces spans the whole invocation,
            // which stands for every declaration it produces.
            auto range =
                unit.context().getSourceManager().getExpansionRange(decl->getSourceRange());
            if(auto [fid, local] = unit.decompose_range(range.getAsRange());
               fid == unit.main_file() && (usings.empty() || usings.back() != local)) {
                usings.push_back(local);
            }
            return;
        }

        if(const auto* ns = llvm::dyn_cast<clang::NamespaceDecl>(decl)) {
            add_block(ns, ns->getRBraceLoc(), "namespace");
            return;
        }

        if(const auto* linkage = llvm::dyn_cast<clang::LinkageSpecDecl>(decl)) {
            if(linkage->hasBraces()) {
                add_block(linkage, linkage->getRBraceLoc(), "linkageSpec");
            }
            return;
        }

        if(const auto* exported = llvm::dyn_cast<clang::ExportDecl>(decl)) {
            if(exported->hasBraces()) {
                add_block(exported, exported->getRBraceLoc(), "export");
            }
            return;
        }

        if(const auto* tag = llvm::dyn_cast<clang::TagDecl>(decl)) {
            if(!tag->isThisDeclarationADefinition()) {
                return;
            }

            add_declaration_block(tag->getBraceRange(),
                                  tag->getLocation(),
                                  *declaration_fold_kind(SymbolKind::from(tag)));

            if(const auto* record = llvm::dyn_cast<clang::CXXRecordDecl>(tag);
               record && !record->isLambda() && !record->isImplicit()) {
                collect_access_specifiers(record);
            }
            return;
        }

        if(const auto* function = llvm::dyn_cast<clang::FunctionDecl>(decl)) {
            collect_parameter_list(function);
            if(function->doesThisDeclarationHaveABody()) {
                add_declaration_block(function->getBody()->getSourceRange(),
                                      function->getLocation(),
                                      *declaration_fold_kind(SymbolKind::from(function)));
            }
        }
    }

    void collect_stmt(const clang::Stmt* stmt, std::uint32_t parent) {
        if(const auto* lambda = llvm::dyn_cast<clang::LambdaExpr>(stmt)) {
            add_range(lambda->getIntroducerRange(), "lambdaCapture", "[...]");
            if(!lambda->getExplicitTemplateParameters().empty()) {
                add_template_parameters(lambda->getTemplateParameterList());
            }
            if(lambda->hasExplicitParameters()) {
                collect_parameter_list(lambda->getCallOperator());
            }
            return;
        }

        if(const auto* compound = llvm::dyn_cast<clang::CompoundStmt>(stmt)) {
            // A function's body already folds as functionBody at its decl;
            // every other written block folds on its own braces. A coroutine
            // stores its written block behind a CoroutineBodyStmt wrapper
            // sharing the same braces — suppress the compound only when that
            // wrapper is itself a function's body; a coroutine lambda has no
            // functionBody producer, so its block must keep folding here.
            if(parent != Semantics::invalid) {
                const Semantics::Node& parent_entry = unit.semantics().node(parent);
                if(const auto* function = parent_entry.node.get<clang::FunctionDecl>();
                   function && function->getBody() == compound) {
                    return;
                }
                if(const auto* coroutine = parent_entry.node.get<clang::CoroutineBodyStmt>();
                   coroutine && coroutine->getBody() == compound &&
                   parent_entry.parent != Semantics::invalid) {
                    const auto* function =
                        unit.semantics().node(parent_entry.parent).node.get<clang::FunctionDecl>();
                    if(function && function->getBody() == coroutine) {
                        return;
                    }
                }
            }
            add_range(compound->getSourceRange(), "compoundStmt", "{...}");
            return;
        }

        if(const auto* call = llvm::dyn_cast<clang::CallExpr>(stmt)) {
            auto tokens = unit.expanded_tokens(call->getSourceRange());
            if(tokens.empty() || tokens.back().kind() != clang::tok::r_paren) {
                return;
            }

            // The callee may itself contain parens; match the right paren
            // backwards to find the argument list's left paren.
            auto right_paren = tokens.back().location();
            std::size_t depth = 0;
            while(!tokens.empty()) {
                auto kind = tokens.back().kind();
                if(kind == clang::tok::r_paren) {
                    depth += 1;
                } else if(kind == clang::tok::l_paren) {
                    depth -= 1;
                    if(depth == 0) {
                        add_range(clang::SourceRange(tokens.back().location(), right_paren),
                                  "functionCall",
                                  "(...)");
                        break;
                    }
                }
                tokens = tokens.drop_back();
            }
            return;
        }

        if(const auto* construct = llvm::dyn_cast<clang::CXXConstructExpr>(stmt)) {
            if(auto parens = construct->getParenOrBraceRange(); parens.isValid()) {
                // Brace-form construction renders as an initializer. When an
                // initializer-list constructor is chosen, the nested
                // InitListExpr shares these braces and produces an identical
                // entry, which the post-sort deduplication removes.
                if(construct->isListInitialization()) {
                    add_range(parens, "initializer", "{...}");
                } else {
                    add_range(parens, "functionCall", "(...)");
                }
            }
            return;
        }

        if(const auto* init = llvm::dyn_cast<clang::InitListExpr>(stmt)) {
            add_range(clang::SourceRange(init->getLBraceLoc(), init->getRBraceLoc()),
                      "initializer",
                      "{...}");
        }
    }

    void collect_access_specifiers(const clang::CXXRecordDecl* record) {
        const clang::AccessSpecDecl* previous = nullptr;
        auto close = [&](clang::SourceLocation next) {
            if(!previous) {
                return;
            }
            auto [header_fid, header] =
                unit.decompose_location(unit.file_location(previous->getColonLoc()));
            auto [next_fid, end] = unit.decompose_location(unit.file_location(next));
            if(header_fid == unit.main_file() && next_fid == unit.main_file()) {
                add_section(line_text_end(header), end, "accessSpecifier");
            }
        };
        for(auto* member: record->decls()) {
            if(auto* access = llvm::dyn_cast<clang::AccessSpecDecl>(member)) {
                close(access->getAccessSpecifierLoc());
                previous = access;
            }
        }
        close(record->getBraceRange().getEnd());
    }

    /// A template's own parameter list, and the outer lists an out-of-line
    /// member definition repeats for its enclosing templates.
    void collect_template_parameters(const clang::Decl* decl) {
        if(const auto* templated = llvm::dyn_cast<clang::TemplateDecl>(decl)) {
            add_template_parameters(templated->getTemplateParameters());
        } else if(const auto* partial =
                      llvm::dyn_cast<clang::ClassTemplatePartialSpecializationDecl>(decl)) {
            add_template_parameters(partial->getTemplateParameters());
        } else if(const auto* partial =
                      llvm::dyn_cast<clang::VarTemplatePartialSpecializationDecl>(decl)) {
            add_template_parameters(partial->getTemplateParameters());
        }

        llvm::ArrayRef<clang::TemplateParameterList*> outer;
        if(const auto* declarator = llvm::dyn_cast<clang::DeclaratorDecl>(decl)) {
            outer = declarator->getTemplateParameterLists();
        } else if(const auto* tag = llvm::dyn_cast<clang::TagDecl>(decl)) {
            outer = tag->getTemplateParameterLists();
        }
        for(const auto* parameters: outer) {
            add_template_parameters(parameters);
        }
    }

    void add_template_parameters(const clang::TemplateParameterList* parameters) {
        add_range(clang::SourceRange(parameters->getLAngleLoc(), parameters->getRAngleLoc()),
                  "templateParams",
                  "<...>");
    }

    void collect_parameter_list(const clang::FunctionDecl* function) {
        if(auto type = function->getFunctionTypeLoc()) {
            add_range(type.getParensRange(), "functionParams", "(...)");
        }
    }

    void collect_block_directives(llvm::ArrayRef<LexicalInfo::BlockDirective> directives) {
        using enum LexicalInfo::BlockDirective::Kind;
        llvm::SmallVector<const LexicalInfo::BlockDirective*> branches;
        llvm::SmallVector<const LexicalInfo::BlockDirective*> regions;
        for(const auto& directive: directives) {
            switch(directive.kind) {
                case If: branches.push_back(&directive); break;
                case Else:
                case EndIf: {
                    if(branches.empty()) {
                        break;
                    }
                    add_section(branches.back()->range.end,
                                directive.range.begin,
                                "conditionDirective");
                    if(directive.kind == Else) {
                        branches.back() = &directive;
                    } else {
                        branches.pop_back();
                    }
                    break;
                }
                case Region: regions.push_back(&directive); break;
                case EndRegion: {
                    if(!regions.empty()) {
                        add_section(regions.pop_back_val()->range.end,
                                    directive.range.begin,
                                    protocol::FoldingRangeKind::Region);
                    }
                    break;
                }
            }
        }
    }

    /// The global module fragment runs to the module declaration, the
    /// private one to the end of the file.
    void collect_module_fragments(llvm::ArrayRef<LexicalInfo::ModuleDeclaration> modules) {
        for(auto [index, module]: llvm::enumerate(modules)) {
            if(module.kind == LexicalInfo::ModuleDeclaration::Kind::Declaration) {
                continue;
            }
            auto end = static_cast<std::uint32_t>(content.size());
            if(index + 1 < modules.size()) {
                const auto& next = modules[index + 1];
                end = next.export_keyword.valid() ? next.export_keyword.begin : next.keyword.begin;
            }
            add_section(line_text_end(module.keyword.begin), end, "moduleFragment");
        }
    }

    void collect_comments(llvm::ArrayRef<LexicalInfo::Comment> comments) {
        llvm::SmallVector<LocalSourceRange> line_comments;
        for(const auto& comment: comments) {
            if(comment.kind == LexicalInfo::Comment::Kind::Line) {
                line_comments.push_back(comment.range);
            } else if(begins_line(comment.range.begin)) {
                // A block comment trailing code would cut across the folds
                // that start at the end of that code's line.
                add_range(comment.range, protocol::FoldingRangeKind::Comment, "/*...*/");
            }
        }
        add_runs(line_comments, protocol::FoldingRangeKind::Comment);
    }

    /// A raw string folds on its delimiters, which may carry a custom
    /// delimiter, an encoding prefix and a literal suffix.
    void collect_raw_strings(llvm::ArrayRef<LocalSourceRange> literals) {
        for(auto literal: literals) {
            auto text = content.substr(literal.begin, literal.length());
            add_range(literal,
                      "rawString",
                      std::format("{}...{}",
                                  text.take_front(text.find('(') + 1),
                                  text.drop_front(text.rfind(')'))));
        }
    }

    /// A brace block whose declaration records only its closing brace: the
    /// opening one is the declaration's first `{`.
    void add_block(const clang::Decl* decl,
                   clang::SourceLocation right_brace,
                   protocol::FoldingRangeKind kind) {
        auto tokens = unit.expanded_tokens(decl->getSourceRange())
                          .drop_until([](const clang::syntax::Token& token) {
                              return token.kind() == clang::tok::l_brace;
                          });
        if(!tokens.empty()) {
            add_declaration_block(clang::SourceRange(tokens.front().location(), right_brace),
                                  decl->getBeginLoc(),
                                  std::move(kind));
        }
    }

    /// `head` is the declaration's name, or the keyword opening a
    /// namespace, linkage or export block (declaration_lines).
    void add_declaration_block(clang::SourceRange braces,
                               clang::SourceLocation head,
                               protocol::FoldingRangeKind kind) {
        auto* fold = add_range(braces, std::move(kind), "{...}");
        if(!fold) {
            return;
        }
        auto [fid, offset] = unit.decompose_location(unit.file_location(head));
        if(fid == unit.main_file()) {
            fold->lines = declaration_lines(content, fold->range, offset, block_directives);
        }
    }

    /// The fold added, if any.
    FoldingRange* add_range(clang::SourceRange range,
                            std::optional<protocol::FoldingRangeKind> kind,
                            std::string collapsed_text) {
        if(range.isInvalid()) {
            return nullptr;
        }

        // What macro arguments spell folds where it is written; what a
        // macro body produces folds at the invocation.
        auto begin = unit.file_location(range.getBegin());
        auto end = unit.file_location(range.getEnd());
        if(begin == end) {
            return nullptr;
        }

        auto [fid, local] = unit.decompose_range(clang::SourceRange(begin, end));
        if(fid != unit.main_file() || !local.valid() || local.end <= local.begin) {
            return nullptr;
        }
        return add_range(local, std::move(kind), std::move(collapsed_text));
    }

    FoldingRange* add_range(LocalSourceRange range,
                            std::optional<protocol::FoldingRangeKind> kind,
                            std::string collapsed_text) {
        // Single-line ranges are not worth folding.
        if(!content.substr(range.begin, range.length()).contains('\n')) {
            return nullptr;
        }

        return &ranges.emplace_back(FoldingRange{
            .range = range,
            .kind = std::move(kind),
            .collapsed_text = std::move(collapsed_text),
        });
    }

    /// Folds each run of two or more items on consecutive lines below the
    /// run's first line. Only an item alone on its lines, trailed by nothing
    /// but its `;` and a line comment, joins a run. Nothing closes a run on
    /// its last line, so a line-folding client hides that line too.
    void add_runs(llvm::ArrayRef<LocalSourceRange> items, protocol::FoldingRangeKind kind) {
        std::optional<LocalSourceRange> first;
        LocalSourceRange last;
        auto flush = [&] {
            if(first && last != *first) {
                auto end = line_text_end(last.end);
                ranges.push_back({
                    .range = {line_text_end(first->begin), end},
                    .kind = kind,
                    .lines = LocalSourceRange{first->begin,                end},
                });
            }
        };

        for(auto item: items) {
            auto rest = content.slice(item.end, line_text_end(item.end)).ltrim(" \t;");
            if(!begins_line(item.begin) || !(rest.empty() || rest.starts_with("//"))) {
                continue;
            }
            if(first && line_begin(content, item.begin) == line_end(content, last.end)) {
                last = item;
                continue;
            }
            flush();
            first = last = item;
        }
        flush();
    }

    /// Whether only blanks precede `offset` on its line.
    bool begins_line(std::uint32_t offset) {
        return content.slice(line_begin(content, offset), offset).trim(" \t").empty();
    }

    /// `begin` ends a header line; a section hiding no whole line is noise.
    void add_section(std::uint32_t begin, std::uint32_t end, protocol::FoldingRangeKind kind) {
        if(end <= begin || content.substr(begin, end - begin).count('\n') < 2) {
            return;
        }
        ranges.push_back({
            .range = {begin, end},
            .kind = std::move(kind)
        });
    }

    /// Where the text of the line holding `offset` ends.
    std::uint32_t line_text_end(std::uint32_t offset) {
        return static_cast<std::uint32_t>(
            std::min(content.find_first_of("\r\n", offset), content.size()));
    }

    CompilationUnitRef unit;
    llvm::StringRef content;
    std::vector<FoldingRange> ranges;

    /// Where each conditional or region directive begins, in source order.
    std::vector<std::uint32_t> block_directives;

    /// Using declarations and directives met on the walk, in source order.
    llvm::SmallVector<LocalSourceRange> usings;
};

}  // namespace

auto folding_ranges(CompilationUnitRef unit) -> std::vector<FoldingRange> {
    return FoldingRangeCollector(unit).collect();
}

auto declaration_fold_kind(SymbolKind kind) -> std::optional<protocol::FoldingRangeKind> {
    switch(kind) {
        case SymbolKind::Namespace: return "namespace";
        case SymbolKind::Class: return "class";
        case SymbolKind::Struct: return "struct";
        case SymbolKind::Union: return "union";
        case SymbolKind::Enum: return "enum";
        case SymbolKind::Function:
        case SymbolKind::Method:
        case SymbolKind::Operator: return "functionBody";
        default: return std::nullopt;
    }
}

auto declaration_lines(llvm::StringRef content,
                       LocalSourceRange block,
                       std::uint32_t head,
                       llvm::ArrayRef<std::uint32_t> block_directives)
    -> std::optional<LocalSourceRange> {
    if(content.slice(block.begin, block.end).count('\n') < 2 ||
       line_begin(content, head) >= line_begin(content, block.begin)) {
        return std::nullopt;
    }
    auto next = std::ranges::lower_bound(block_directives, head);
    if(next != block_directives.end() && *next <= block.begin) {
        return std::nullopt;
    }
    return LocalSourceRange{head, line_begin(content, block.end) - 1};
}

auto folding_ranges_to_protocol(llvm::ArrayRef<FoldingRange> ranges,
                                const PositionMap& map,
                                bool line_folding_only) -> std::vector<protocol::FoldingRange> {
    std::vector<protocol::FoldingRange> result;
    result.reserve(ranges.size());

    for(const auto& item: ranges) {
        auto bounds = line_folding_only ? item.lines.value_or(item.range) : item.range;
        auto start = map.to_position(bounds.begin);
        auto end = map.to_position(bounds.end);
        if(!start || !end)
            continue;

        protocol::FoldingRange range;
        if(line_folding_only) {
            // The client hides whole lines below the start line. The line a
            // fold's range ends on holds its closing delimiter or the next
            // header — `} else {`, `#else`, `private:` — and must stay
            // visible; `lines` ends on its last hidden line instead.
            auto shown = item.lines ? end->line + 1 : end->line;
            if(shown <= start->line + 1) {
                continue;
            }
            range = {.start_line = start->line, .end_line = shown - 1};
        } else {
            range = {
                .start_line = start->line,
                .start_character = start->character,
                .end_line = end->line,
                .end_character = end->character,
            };
        }

        if(item.kind.has_value()) {
            range.kind = *item.kind;
        }

        if(!item.collapsed_text.empty()) {
            range.collapsed_text = item.collapsed_text;
        }

        result.push_back(std::move(range));
    }

    // VS Code keeps only the first fold it sees starting on a line. A body
    // starting on its declaration's line must win over the parameter list
    // folding from that line, so outer folds go first.
    if(line_folding_only) {
        std::ranges::stable_sort(
            result,
            [](const protocol::FoldingRange& lhs, const protocol::FoldingRange& rhs) {
                return std::tie(lhs.start_line, rhs.end_line) <
                       std::tie(rhs.start_line, lhs.end_line);
            });
    }

    return result;
}

}  // namespace clice::feature

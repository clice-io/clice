#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "compile/compilation_unit.h"
#include "feature/feature.h"
#include "semantic/decls.h"
#include "semantic/semantics.h"

#include "llvm/Support/Casting.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclCXX.h"
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
/// region) runs from the end of its header line to the next header, which
/// stays visible.
class FoldingRangeCollector {
public:
    explicit FoldingRangeCollector(CompilationUnitRef unit) :
        unit(unit), content(unit.main_content()) {}

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
        if(const auto* ns = llvm::dyn_cast<clang::NamespaceDecl>(decl)) {
            // NamespaceDecl does not store its left brace location; scan for
            // it so the fold keeps the name visible.
            auto tokens = unit.expanded_tokens(ns->getSourceRange())
                              .drop_until([](const clang::syntax::Token& token) {
                                  return token.kind() == clang::tok::l_brace;
                              });
            if(!tokens.empty()) {
                add_range(clang::SourceRange(tokens.front().location(), ns->getRBraceLoc()),
                          "namespace",
                          "{...}");
            }
            return;
        }

        if(const auto* tag = llvm::dyn_cast<clang::TagDecl>(decl)) {
            if(!tag->isThisDeclarationADefinition()) {
                return;
            }

            std::string_view kind = tag->isStruct()  ? "struct"
                                    : tag->isClass() ? "class"
                                    : tag->isUnion() ? "union"
                                                     : "enum";
            add_range(tag->getBraceRange(), kind, "{...}");

            if(const auto* record = llvm::dyn_cast<clang::CXXRecordDecl>(tag);
               record && !record->isLambda() && !record->isImplicit()) {
                collect_access_specifiers(record);
            }
            return;
        }

        if(const auto* function = llvm::dyn_cast<clang::FunctionDecl>(decl)) {
            if(!function->doesThisDeclarationHaveABody()) {
                collect_parameter_list(function->getSourceRange());
                return;
            }

            collect_parameter_list(function->getBeginLoc(), function->getBody()->getBeginLoc());
            add_range(function->getBody()->getSourceRange(), "functionBody", "{...}");
        }
    }

    void collect_stmt(const clang::Stmt* stmt, std::uint32_t parent) {
        if(const auto* lambda = llvm::dyn_cast<clang::LambdaExpr>(stmt)) {
            add_range(lambda->getIntroducerRange(), "lambdaCapture", "[...]");
            if(lambda->hasExplicitParameters()) {
                collect_parameter_list(lambda->getIntroducerRange().getEnd(),
                                       lambda->getCompoundStmtBody()->getBeginLoc());
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
            if(previous) {
                add_section(previous->getColonLoc(), next, "accessSpecifier");
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

    void collect_parameter_list(clang::SourceLocation left, clang::SourceLocation right) {
        collect_parameter_list(clang::SourceRange(left, right));
    }

    void collect_parameter_list(clang::SourceRange bounds) {
        auto tokens = unit.expanded_tokens(bounds);
        auto left_paren = tokens.drop_until(
            [](const clang::syntax::Token& token) { return token.kind() == clang::tok::l_paren; });
        if(left_paren.empty()) {
            return;
        }

        auto right_paren = std::find_if(
            left_paren.rbegin(),
            left_paren.rend(),
            [](const clang::syntax::Token& token) { return token.kind() == clang::tok::r_paren; });
        if(right_paren == left_paren.rend()) {
            return;
        }

        add_range(clang::SourceRange(left_paren.front().location(), right_paren->location()),
                  "functionParams",
                  "(...)");
    }

    void collect_block_directives(llvm::ArrayRef<LexicalInfo::BlockDirective> directives) {
        using enum LexicalInfo::BlockDirective::Kind;
        // Each branch folds up to the directive that ends it, so every arm
        // of a chain folds on its own.
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
                                    protocol::FoldingRangeKind::region);
                    }
                    break;
                }
            }
        }
    }

    void add_range(clang::SourceRange range,
                   std::optional<protocol::FoldingRangeKind> kind,
                   std::string collapsed_text) {
        if(range.isInvalid()) {
            return;
        }

        auto [begin, end] = range;
        begin = unit.expansion_location(begin);
        end = unit.expansion_location(end);
        if(begin == end) {
            return;
        }

        auto [fid, local] = unit.decompose_range(clang::SourceRange(begin, end));
        if(fid != unit.main_file() || !local.valid() || local.end <= local.begin) {
            return;
        }

        // Single-line ranges are not worth folding.
        if(!content.substr(local.begin, local.length()).contains('\n')) {
            return;
        }

        ranges.push_back({
            .range = local,
            .kind = std::move(kind),
            .collapsed_text = std::move(collapsed_text),
        });
    }

    void add_section(clang::SourceLocation header,
                     clang::SourceLocation next,
                     protocol::FoldingRangeKind kind) {
        auto [header_fid, header_offset] = unit.decompose_location(unit.file_location(header));
        auto [next_fid, next_offset] = unit.decompose_location(unit.file_location(next));
        if(header_fid == unit.main_file() && next_fid == unit.main_file()) {
            add_section(header_end(header_offset), next_offset, std::move(kind));
        }
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
    std::uint32_t header_end(std::uint32_t offset) {
        return static_cast<std::uint32_t>(
            std::min(content.find_first_of("\r\n", offset), content.size()));
    }

    CompilationUnitRef unit;
    llvm::StringRef content;
    std::vector<FoldingRange> ranges;
};

}  // namespace

auto folding_ranges(CompilationUnitRef unit) -> std::vector<FoldingRange> {
    return FoldingRangeCollector(unit).collect();
}

auto folding_ranges_to_protocol(llvm::ArrayRef<FoldingRange> ranges,
                                llvm::StringRef content,
                                llvm::ArrayRef<std::uint32_t> line_starts,
                                PositionEncoding encoding,
                                bool line_folding_only) -> std::vector<protocol::FoldingRange> {
    LineMap map(content,
                std::span<const std::uint32_t>(line_starts.data(), line_starts.size()),
                encoding);

    std::vector<protocol::FoldingRange> result;
    result.reserve(ranges.size());

    for(const auto& item: ranges) {
        auto start = to_position(map, item.range.begin);
        auto end = to_position(map, item.range.end);
        if(!start || !end)
            continue;

        protocol::FoldingRange range;
        if(line_folding_only) {
            // The client hides whole lines below the start line. The line a
            // fold ends on holds its closing delimiter or the next header —
            // `} else {`, `#else`, `private:` — and must stay visible.
            if(end->line <= start->line + 1) {
                continue;
            }
            range = {.start_line = start->line, .end_line = end->line - 1};
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

    return result;
}

}  // namespace clice::feature

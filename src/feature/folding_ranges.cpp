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
/// the DFS pre-order record of the interested file's written AST — instead of
/// running another RecursiveASTVisitor over the TU. Folding needs no nesting
/// state: every recorded decl and stmt contributes its ranges independently.
///
/// Fold kinds are plain strings on the wire (LSP standardizes only `comment`,
/// `imports` and `region`; servers may add custom values).
class FoldingRangeCollector {
public:
    explicit FoldingRangeCollector(CompilationUnitRef unit) : unit(unit) {}

    auto collect() -> std::vector<FoldingRange> {
        auto nodes = unit.semantics().node_entries();
        std::uint32_t index = 0;
        while(index < nodes.size()) {
            const Semantics::Node& entry = nodes[index];
            if(!entry.node.is_ast()) {
                // The preprocessor segment follows the AST segment; directive
                // folds are collected from the unit's directive table below.
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

        auto directives_it = unit.directives().find(unit.interested_file());
        if(directives_it != unit.directives().end()) {
            collect_condition_directives(directives_it->second.conditions);
            collect_pragma_region(directives_it->second.pragmas);
        }

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
        clang::AccessSpecDecl* previous = nullptr;
        for(auto* member: record->decls()) {
            auto* access = llvm::dyn_cast<clang::AccessSpecDecl>(member);
            if(!access) {
                continue;
            }

            if(previous) {
                add_range(
                    clang::SourceRange(previous->getColonLoc(), access->getAccessSpecifierLoc()),
                    "accessSpecifier",
                    "");
            }
            previous = access;
        }

        if(previous) {
            add_range(clang::SourceRange(previous->getColonLoc(), record->getBraceRange().getEnd()),
                      "accessSpecifier",
                      "");
        }
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

    void collect_condition_directives(const std::vector<Condition>& conditions) {
        llvm::SmallVector<const Condition*> stack;

        for(const auto& condition: conditions) {
            switch(condition.kind) {
                case Condition::BranchKind::If:
                case Condition::BranchKind::Ifdef:
                case Condition::BranchKind::Ifndef:
                case Condition::BranchKind::Elif:
                case Condition::BranchKind::Elifndef: stack.push_back(&condition); break;

                case Condition::BranchKind::Else: {
                    if(!stack.empty()) {
                        auto* previous = stack.pop_back_val();
                        add_range(
                            clang::SourceRange(previous->condition_range.getEnd(), condition.loc),
                            "conditionDirective",
                            "");
                    }
                    stack.push_back(&condition);
                    break;
                }

                case Condition::BranchKind::EndIf:
                    if(!stack.empty()) {
                        (void)stack.pop_back_val();
                    }
                    break;

                default: break;
            }
        }
    }

    void collect_pragma_region(const std::vector<Pragma>& pragmas) {
        llvm::SmallVector<const Pragma*> stack;

        for(const auto& pragma: pragmas) {
            if(pragma.kind == Pragma::Kind::Region) {
                stack.push_back(&pragma);
                continue;
            }

            if(pragma.kind != Pragma::Kind::EndRegion || stack.empty()) {
                continue;
            }

            auto* previous = stack.pop_back_val();
            add_range(clang::SourceRange(previous->loc, pragma.loc),
                      protocol::FoldingRangeKind::region,
                      "");
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
        if(fid != unit.interested_file() || !local.valid() || local.end <= local.begin) {
            return;
        }

        // Single-line ranges are not worth folding.
        auto content = unit.file_content(fid);
        if(!content.substr(local.begin, local.end - local.begin).contains('\n')) {
            return;
        }

        ranges.push_back({
            .range = local,
            .kind = std::move(kind),
            .collapsed_text = std::move(collapsed_text),
        });
    }

    CompilationUnitRef unit;
    std::vector<FoldingRange> ranges;
};

}  // namespace

auto folding_ranges(CompilationUnitRef unit) -> std::vector<FoldingRange> {
    return FoldingRangeCollector(unit).collect();
}

auto folding_ranges(CompilationUnitRef unit, PositionEncoding encoding)
    -> std::vector<protocol::FoldingRange> {
    auto collected = folding_ranges(unit);
    LineMap map(unit.interested_content(), unit.line_starts(), encoding);

    std::vector<protocol::FoldingRange> result;
    result.reserve(collected.size());

    for(const auto& item: collected) {
        auto start = to_position(map, item.range.begin);
        auto end = to_position(map, item.range.end);
        if(!start || !end)
            continue;

        protocol::FoldingRange range{
            .start_line = start->line,
            .start_character = start->character,
            .end_line = end->line,
            .end_character = end->character,
        };

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

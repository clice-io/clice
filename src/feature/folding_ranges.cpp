#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "feature/feature.h"
#include "semantic/filtered_ast_visitor.h"

#include "clang/AST/ExprCXX.h"

namespace clice::feature {

namespace {

namespace protocol = eventide::language::protocol;

struct RawFoldingRange {
    LocalSourceRange range;
    std::optional<protocol::FoldingRangeKind> kind;
    std::string collapsed_text;
};

class FoldingRangeCollector : public FilteredASTVisitor<FoldingRangeCollector> {
public:
    explicit FoldingRangeCollector(CompilationUnitRef unit) : FilteredASTVisitor(unit, true) {}

    bool VisitNamespaceDecl(const clang::NamespaceDecl* decl) {
        auto tokens = unit.expanded_tokens(decl->getSourceRange())
                          .drop_until([](const clang::syntax::Token& token) {
                              return token.kind() == clang::tok::l_brace;
                          });

        if(tokens.empty()) {
            return true;
        }

        add_range(clang::SourceRange(tokens.front().location(), decl->getRBraceLoc()),
                  std::nullopt,
                  "{...}");
        return true;
    }

    bool VisitTagDecl(const clang::TagDecl* decl) {
        if(!decl->isThisDeclarationADefinition()) {
            return true;
        }

        add_range(decl->getBraceRange(), std::nullopt, "{...}");

        auto* record = llvm::dyn_cast<clang::CXXRecordDecl>(decl);
        if(!record || record->isLambda() || record->isImplicit()) {
            return true;
        }

        clang::AccessSpecDecl* previous = nullptr;
        for(auto* member: record->decls()) {
            auto* access = llvm::dyn_cast<clang::AccessSpecDecl>(member);
            if(!access) {
                continue;
            }

            if(previous) {
                add_range(
                    clang::SourceRange(previous->getColonLoc(), access->getAccessSpecifierLoc()),
                    std::nullopt,
                    "");
            }
            previous = access;
        }

        if(previous) {
            add_range(clang::SourceRange(previous->getColonLoc(), record->getBraceRange().getEnd()),
                      std::nullopt,
                      "");
        }

        return true;
    }

    bool VisitFunctionDecl(const clang::FunctionDecl* decl) {
        if(!decl->doesThisDeclarationHaveABody()) {
            collect_parameter_list(decl->getSourceRange());
            return true;
        }

        collect_parameter_list(decl->getBeginLoc(), decl->getBody()->getBeginLoc());
        add_range(decl->getBody()->getSourceRange(), std::nullopt, "{...}");
        return true;
    }

    bool VisitLambdaExpr(const clang::LambdaExpr* lambda) {
        add_range(lambda->getIntroducerRange(), std::nullopt, "[...]");

        if(lambda->hasExplicitParameters()) {
            collect_parameter_list(lambda->getIntroducerRange().getEnd(),
                                   lambda->getCompoundStmtBody()->getBeginLoc());
        }

        collect_compound_stmt(lambda->getBody());
        return true;
    }

    bool VisitCallExpr(const clang::CallExpr* call) {
        auto tokens = unit.expanded_tokens(call->getSourceRange());
        if(tokens.empty() || tokens.back().kind() != clang::tok::r_paren) {
            return true;
        }

        auto right_paren = tokens.back().location();
        std::size_t depth = 0;

        while(!tokens.empty()) {
            auto kind = tokens.back().kind();
            if(kind == clang::tok::r_paren) {
                depth += 1;
            } else if(kind == clang::tok::l_paren && --depth == 0) {
                add_range(clang::SourceRange(tokens.back().location(), right_paren),
                          std::nullopt,
                          "(...)");
                break;
            }
            tokens = tokens.drop_back();
        }

        return true;
    }

    bool VisitCXXConstructExpr(const clang::CXXConstructExpr* expr) {
        if(auto paren_or_brace = expr->getParenOrBraceRange(); paren_or_brace.isValid()) {
            add_range(clang::SourceRange(paren_or_brace.getBegin().getLocWithOffset(1),
                                         paren_or_brace.getEnd()),
                      std::nullopt,
                      "(...)");
        }
        return true;
    }

    bool VisitInitListExpr(const clang::InitListExpr* expr) {
        add_range(clang::SourceRange(expr->getLBraceLoc(), expr->getRBraceLoc()),
                  std::nullopt,
                  "{...}");
        return true;
    }

    auto collect() -> std::vector<RawFoldingRange> {
        TraverseDecl(unit.tu());

        auto directives_it = unit.directives().find(unit.interested_file());
        if(directives_it != unit.directives().end()) {
            collect_directives(directives_it->second);
        }

        std::ranges::sort(ranges, [](const RawFoldingRange& lhs, const RawFoldingRange& rhs) {
            if(lhs.range.begin != rhs.range.begin) {
                return lhs.range.begin < rhs.range.begin;
            }
            return lhs.range.end < rhs.range.end;
        });

        return std::move(ranges);
    }

private:
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

        auto content = unit.file_content(fid);
        bool single_line = true;
        for(std::uint32_t offset = local.begin; offset < local.end; ++offset) {
            if(content[offset] == '\n') {
                single_line = false;
                break;
            }
        }

        if(single_line) {
            return;
        }

        ranges.push_back({
            .range = local,
            .kind = std::move(kind),
            .collapsed_text = std::move(collapsed_text),
        });
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
                  std::nullopt,
                  "(...)");
    }

    void collect_compound_stmt(const clang::Stmt* stmt) {
        auto* compound = llvm::dyn_cast_or_null<clang::CompoundStmt>(stmt);
        if(!compound) {
            return;
        }

        add_range(clang::SourceRange(compound->getLBracLoc(), compound->getRBracLoc()),
                  std::nullopt,
                  "{...}");

        for(const auto* child: stmt->children()) {
            collect_compound_stmt(child);
        }
    }

    void collect_directives(const Directive& directives) {
        collect_condition_directives(directives.conditions);
        collect_pragma_region(directives.pragmas);
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
                            std::nullopt,
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
                      protocol::FoldingRangeKind(protocol::FoldingRangeKind::region),
                      "");
        }
    }

private:
    std::vector<RawFoldingRange> ranges;
};

}  // namespace

auto folding_ranges(CompilationUnitRef unit, PositionEncoding encoding)
    -> std::vector<protocol::FoldingRange> {
    auto collected = FoldingRangeCollector(unit).collect();
    PositionConverter converter(unit.interested_content(), encoding);

    std::vector<protocol::FoldingRange> result;
    result.reserve(collected.size());

    for(const auto& item: collected) {
        auto start = converter.to_position(item.range.begin);
        auto end = converter.to_position(item.range.end);

        protocol::FoldingRange range{
            .start_line = start.line,
            .start_character = start.character,
            .end_line = end.line,
            .end_character = end.character,
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

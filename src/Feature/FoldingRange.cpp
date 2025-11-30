#include "Feature/FoldingRange.h"

#include "AST/FilterASTVisitor.h"
#include "Compiler/Compilation.h"
#include "Support/Compare.h"

namespace clice::feature {

namespace {

class FoldingRangeCollector : public FilteredASTVisitor<FoldingRangeCollector> {
public:
    FoldingRangeCollector(CompilationUnit& unit, bool interested_only) :
        FilteredASTVisitor(unit, interested_only) {}

    constexpr static auto LastColOfLine = std::numeric_limits<unsigned>::max();

    bool VisitNamespaceDecl(const clang::NamespaceDecl* decl) {
        // Find first '{' in namespace declaration.
        auto shrink = unit.expanded_tokens(decl->getSourceRange())
                          .drop_until([](const clang::syntax::Token& token) -> bool {
                              return token.kind() == clang::tok::l_brace;
                          });

        /// If The AST is not complete, we may cannot find the '{'.
        if(shrink.empty()) {
            return true;
        }

        /// Collect namespace.
        clang::SourceRange range(shrink.front().location(), decl->getRBraceLoc());
        add_range(range, FoldingRangeKind::Namespace, "{...}");

        return true;
    }

    bool VisitTagDecl(const clang::TagDecl* decl) {
        /// If it's a forward declaration, nothing to do.
        if(!decl->isThisDeclarationADefinition()) {
            return true;
        }

        // Collect the definition of class/struct/enum/union.
        FoldingRangeKind kind = decl->isStruct()  ? FoldingRangeKind::Struct
                                : decl->isClass() ? FoldingRangeKind::Class
                                : decl->isUnion() ? FoldingRangeKind::Union
                                                  : FoldingRangeKind::Enum;
        add_range(decl->getBraceRange(), kind, "{...}");

        /// Collect public/protected/private blocks for a non-lambda struct/class.
        if(auto RD = llvm::dyn_cast<clang::CXXRecordDecl>(decl)) {
            if(RD->isLambda() || RD->isImplicit()) {
                return true;
            }

            clang::AccessSpecDecl* last = nullptr;
            for(auto* decl: RD->decls()) {
                if(auto* AS = llvm::dyn_cast<clang::AccessSpecDecl>(decl)) {
                    if(last) {
                        add_range(
                            clang::SourceRange(last->getColonLoc(), AS->getAccessSpecifierLoc()),
                            FoldingRangeKind::AccessSpecifier,
                            "");
                    }
                    last = AS;
                }
            }

            if(last) {
                add_range(clang::SourceRange(last->getColonLoc(), RD->getBraceRange().getEnd()),
                          FoldingRangeKind::AccessSpecifier,
                          "");
            }
        }

        return true;
    }

    bool VisitFunctionDecl(const clang::FunctionDecl* decl) {
        /// If it's a forward declaration, try to collect the parameter list.
        if(!decl->doesThisDeclarationHaveABody()) {
            collect_parameter_list(decl->getSourceRange());
        } else {
            collect_parameter_list(decl->getBeginLoc(), decl->getBody()->getBeginLoc());

            /// Collect function body.
            add_range(decl->getBody()->getSourceRange(), FoldingRangeKind::FunctionBody, "{...}");
        }

        return true;
    }

    bool VisitLambdaExpr(const clang::LambdaExpr* lambda) {
        auto introduceRange = lambda->getIntroducerRange();
        /// Collect lambda capture list.
        add_range(lambda->getIntroducerRange(), FoldingRangeKind::LambdaCapture, "[...]");

        /// Collect explicit parameter list.
        if(lambda->hasExplicitParameters()) {
            collect_parameter_list(introduceRange.getEnd(),
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
        size_t depth = 0;
        while(!tokens.empty()) {
            auto kind = tokens.back().kind();
            if(kind == clang::tok::r_paren)
                depth += 1;
            else if(kind == clang::tok::l_paren && --depth == 0) {
                add_range({tokens.back().location(), right_paren},
                          FoldingRangeKind::FunctionCall,
                          "(...)");
                break;
            }
            tokens = tokens.drop_back();
        }

        return true;
    }

    bool VisitCXXConstructExpr(const clang::CXXConstructExpr* stmt) {
        if(auto range = stmt->getParenOrBraceRange(); range.isValid()) {
            add_range({range.getBegin().getLocWithOffset(1), range.getEnd()},
                      FoldingRangeKind::FunctionCall,
                      "(...)");
        }
        return true;
    }

    bool VisitInitListExpr(const clang::InitListExpr* expr) {
        add_range({expr->getLBraceLoc(), expr->getRBraceLoc()},
                  FoldingRangeKind::Initializer,
                  "{...}");
        return true;
    }

    auto build_for_file(CompilationUnit& unit) {
        TraverseDecl(unit.tu());
        collect_drectives(unit.directives()[unit.interested_file()]);
        std::ranges::sort(result, refl::less);
        return std::move(result);
    }

    auto build_for_index(CompilationUnit& unit) {
        TraverseDecl(unit.tu());
        for(auto& [fid, directive]: unit.directives()) {
            collect_drectives(directive);
        }

        for(auto& [fid, ranges]: index_result) {
            std::ranges::sort(ranges, refl::less);
        }

        return std::move(index_result);
    }

private:
    void add_range(clang::SourceRange range, FoldingRangeKind kind, std::string text) {
        /// In normal AST, the range must be valid. But unfortunately, the range
        /// may be invalid in incomplete AST, so we need to check it.
        if(range.isInvalid()) {
            return;
        }

        auto [begin, end] = range;
        begin = unit.expansion_location(begin);
        end = unit.expansion_location(end);

        /// If they are from the same macro expansion, skip it.
        if(begin == end) {
            return;
        }

        auto [fid, local_range] = unit.decompose_range(clang::SourceRange(begin, end));
        auto [begin_offset, end_offset] = local_range;

        bool is_same_line = true;
        auto content = unit.file_content(fid);
        for(auto i = begin_offset; i < end_offset; ++i) {
            if(content[i] == '\n') {
                is_same_line = false;
                break;
            }
        }

        /// TODO: Currently, we only support folding range in different lines.
        if(is_same_line) {
            return;
        }

        auto& ranges = interested_only ? result : index_result[fid];
        ranges.emplace_back(local_range, kind, std::move(text));
    }

    void collect_parameter_list(clang::SourceLocation left, clang::SourceLocation right) {
        collect_parameter_list(clang::SourceRange(left, right));
    }

    /// Collect function parameter list between '(' and ')'.
    void collect_parameter_list(clang::SourceRange bounds) {
        auto tokens = unit.expanded_tokens(bounds);
        auto left_paren = tokens.drop_until([](const auto& tk) {  //
            return tk.kind() == clang::tok::l_paren;
        });

        if(left_paren.empty())
            return;

        auto right_paren_iter =
            std::find_if(left_paren.rbegin(), left_paren.rend(), [](const auto& tk) {
                return tk.kind() == clang::tok::r_paren;
            });

        if(right_paren_iter == left_paren.rend())
            return;

        add_range(clang::SourceRange(left_paren.front().location(), right_paren_iter->location()),
                  FoldingRangeKind::FunctionParams,
                  "(...)");
    }

    void collect_compound_stmt(const clang::Stmt* stmt) {
        if(auto* CS = llvm::dyn_cast<clang::CompoundStmt>(stmt)) {
            add_range({CS->getLBracLoc(), CS->getRBracLoc()},
                      FoldingRangeKind::CompoundStmt,
                      "{...}");
            for(auto child: stmt->children()) {
                collect_compound_stmt(child);
            }
        }
    }

    using ASTDirectives =
        std::remove_reference_t<decltype(std::declval<CompilationUnit>().directives())>;

    void collect_drectives(const Directive& directive) {
        collect_condition_directive(directive.conditions);
        collect_pragma_region(directive.pragmas);

        /// TODO:
        /// Collect multiline include statement.
    }

    /// Collect all condition macro's block as folding range.
    void collect_condition_directive(const std::vector<Condition>& conds) {
        // All condition directives have been stored in `conds` variable, ordered by presumed line
        // number increasement, so use a stack to handle the branch structure.
        llvm::SmallVector<const Condition*> stack = {};

        for(auto& cond: conds) {
            switch(cond.kind) {
                case Condition::BranchKind::If:
                case Condition::BranchKind::Ifdef:
                case Condition::BranchKind::Ifndef:
                case Condition::BranchKind::Elif:
                case Condition::BranchKind::Elifndef: {
                    stack.push_back(&cond);
                    break;
                }

                case Condition::BranchKind::Else: {
                    if(!stack.empty()) {
                        auto last = stack.pop_back_val();
                        add_range({last->condition_range.getEnd(), cond.loc},
                                  FoldingRangeKind::ConditionDirective,
                                  "");
                    }

                    stack.push_back(&cond);
                    break;
                }

                case Condition::BranchKind::EndIf: {
                    if(!stack.empty()) {
                        auto last = stack.pop_back_val();

                        // For a directive without condition range e.g #else
                        // its condition range is invalid.
                        if(last->condition_range.isValid()) {
                            /// collect({last->conditionRange.getBegin(), cond.loc}, {0, -1});
                        } else {
                            /// collect({last->loc, cond.loc},
                            ///        {refl::enum_name(cond.kind).length(), -1});
                        }
                    }
                    break;
                }

                default: break;
            }
        }
    }

    /// Collect all condition macro's block as folding range.
    void collect_pragma_region(const std::vector<Pragma>& pragmas) {
        llvm::SmallVector<const Pragma*> stack;
        for(auto& pragma: pragmas) {
            if(pragma.kind == Pragma::Region) {
                stack.push_back(&pragma);
            } else if(pragma.kind == Pragma::EndRegion) {
                if(stack.empty()) {
                    continue;
                }

                auto last = stack.pop_back_val();
                add_range(clang::SourceRange(last->loc, pragma.loc), FoldingRangeKind::Region, "");
            }
        }
    }

private:
    FoldingRanges result;
    index::Shared<FoldingRanges> index_result;
};

}  // namespace

FoldingRanges folding_ranges(CompilationUnit& unit) {
    return FoldingRangeCollector(unit, true).build_for_file(unit);
}

index::Shared<FoldingRanges> index_folding_range(CompilationUnit& unit) {
    return FoldingRangeCollector(unit, false).build_for_index(unit);
}  // namespace feature

}  // namespace clice::feature

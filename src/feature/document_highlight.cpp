#include <algorithm>
#include <optional>
#include <utility>
#include <vector>

#include "feature/feature.h"
#include "semantic/selection.h"

#include "clang/AST/Decl.h"
#include "clang/AST/DeclObjC.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/AST/Stmt.h"
#include "clang/AST/StmtCXX.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Tooling/Syntax/Tokens.h"

namespace clice::feature {

namespace {

const clang::Stmt* function_body(const clang::DynTypedNode& node) {
    if(const auto* decl = node.get<clang::FunctionDecl>())
        return decl->getBody();
    if(const auto* decl = node.get<clang::BlockDecl>())
        return decl->getBody();
    if(const auto* expr = node.get<clang::LambdaExpr>())
        return expr->getBody();
    if(const auto* decl = node.get<clang::ObjCMethodDecl>())
        return decl->getBody();
    return nullptr;
}

const clang::Stmt* loop_body(const clang::DynTypedNode& node) {
    if(const auto* stmt = node.get<clang::ForStmt>())
        return stmt->getBody();
    if(const auto* stmt = node.get<clang::CXXForRangeStmt>())
        return stmt->getBody();
    if(const auto* stmt = node.get<clang::WhileStmt>())
        return stmt->getBody();
    if(const auto* stmt = node.get<clang::DoStmt>())
        return stmt->getBody();
    return nullptr;
}

class ControlFlowCollector : public clang::RecursiveASTVisitor<ControlFlowCollector> {
    enum Target : int {
        Break = 1 << 0,
        Continue = 1 << 1,
        Return = 1 << 2,
        Case = 1 << 3,
        Throw = 1 << 4,
        All = Break | Continue | Return | Case | Throw,
    };

    int ignored = 0;
    clang::SourceRange bounds;
    std::vector<clang::SourceLocation>& result;
    const clang::SourceManager& source_manager;

    template <typename Callback>
    bool filter_and_traverse(const clang::DynTypedNode& node, Callback&& callback) {
        int old_ignored = ignored;
        if(function_body(node)) {
            ignored = All;
        } else if(loop_body(node)) {
            ignored |= Break | Continue;
        } else if(node.get<clang::SwitchStmt>()) {
            ignored |= Break | Case;
        }

        bool keep_going = ignored == All ? true : std::forward<Callback>(callback)();
        ignored = old_ignored;
        return keep_going;
    }

    void found(Target target, clang::SourceLocation location) {
        if((target & ignored) != 0 || !location.isValid())
            return;
        if(location == bounds.getEnd() ||
           source_manager.isBeforeInTranslationUnit(location, bounds.getBegin()) ||
           source_manager.isBeforeInTranslationUnit(bounds.getEnd(), location)) {
            return;
        }
        result.push_back(location);
    }

public:
    ControlFlowCollector(clang::SourceRange bounds,
                         std::vector<clang::SourceLocation>& result,
                         const clang::SourceManager& source_manager) :
        bounds(bounds), result(result), source_manager(source_manager) {}

    bool TraverseDecl(clang::Decl* decl) {
        if(!decl)
            return true;
        return filter_and_traverse(clang::DynTypedNode::create(*decl), [&] {
            return clang::RecursiveASTVisitor<ControlFlowCollector>::TraverseDecl(decl);
        });
    }

    bool TraverseStmt(clang::Stmt* stmt) {
        if(!stmt)
            return true;
        return filter_and_traverse(clang::DynTypedNode::create(*stmt), [&] {
            return clang::RecursiveASTVisitor<ControlFlowCollector>::TraverseStmt(stmt);
        });
    }

    bool VisitReturnStmt(clang::ReturnStmt* stmt) {
        found(Return, stmt->getReturnLoc());
        return true;
    }

    bool VisitBreakStmt(clang::BreakStmt* stmt) {
        found(Break, stmt->getBreakLoc());
        return true;
    }

    bool VisitContinueStmt(clang::ContinueStmt* stmt) {
        found(Continue, stmt->getContinueLoc());
        return true;
    }

    bool VisitSwitchCase(clang::SwitchCase* stmt) {
        found(Case, stmt->getKeywordLoc());
        return true;
    }

    bool VisitCXXThrowExpr(clang::CXXThrowExpr* expr) {
        found(Throw, expr->getThrowLoc());
        return true;
    }
};

clang::SourceRange case_bounds(const clang::SwitchStmt& switch_stmt,
                               clang::SourceLocation location,
                               const clang::SourceManager& source_manager) {
    std::vector<const clang::SwitchCase*> cases;
    for(const clang::SwitchCase* current = switch_stmt.getSwitchCaseList(); current;
        current = current->getNextSwitchCase()) {
        cases.push_back(current);
    }

    std::ranges::sort(cases, [&](const clang::SwitchCase* lhs, const clang::SwitchCase* rhs) {
        return source_manager.isBeforeInTranslationUnit(lhs->getKeywordLoc(), rhs->getKeywordLoc());
    });

    auto after = std::ranges::partition_point(cases, [&](const clang::SwitchCase* current) {
        return !source_manager.isBeforeInTranslationUnit(location, current->getKeywordLoc());
    });

    clang::SourceLocation end =
        after == cases.end() ? switch_stmt.getEndLoc() : (*after)->getKeywordLoc();
    if(after == cases.begin())
        return clang::SourceRange(switch_stmt.getBeginLoc(), end);

    auto before = std::prev(after);
    while(before != cases.begin() && (*std::prev(before))->getSubStmt() == *before) {
        --before;
    }
    return clang::SourceRange((*before)->getKeywordLoc(), end);
}

std::vector<clang::SourceLocation> related_control_flow(const SelectionTree::Node& node) {
    enum class Cursor : std::uint8_t {
        None,
        Break,
        Continue,
        Return,
        Case,
        Throw,
    };

    const auto& source_manager = node.decl_context().getParentASTContext().getSourceManager();
    std::vector<clang::SourceLocation> result;

    Cursor cursor = Cursor::None;
    if(node.get<clang::BreakStmt>()) {
        cursor = Cursor::Break;
    } else if(node.get<clang::ContinueStmt>()) {
        cursor = Cursor::Continue;
    } else if(node.get<clang::ReturnStmt>()) {
        cursor = Cursor::Return;
    } else if(node.get<clang::CXXThrowExpr>()) {
        cursor = Cursor::Throw;
    } else if(node.get<clang::CaseStmt>() || node.get<clang::DefaultStmt>()) {
        cursor = Cursor::Case;
    }

    const clang::Stmt* root = nullptr;
    clang::SourceRange bounds;
    for(const auto* current = &node; current; current = current->parent) {
        if(const clang::Stmt* body = function_body(current->data)) {
            if(cursor == Cursor::Return || cursor == Cursor::Throw)
                root = body;
            break;
        }

        if(const clang::Stmt* body = loop_body(current->data)) {
            if(cursor == Cursor::None || cursor == Cursor::Break || cursor == Cursor::Continue) {
                root = body;
                result.push_back(current->data.getSourceRange().getBegin());
                break;
            }
        }

        if(const auto* switch_stmt = current->get<clang::SwitchStmt>()) {
            if(cursor == Cursor::Break || cursor == Cursor::Case) {
                result.push_back(switch_stmt->getSwitchLoc());
                root = switch_stmt->getBody();
                bounds = case_bounds(*switch_stmt,
                                     node.data.getSourceRange().getBegin(),
                                     source_manager);
                break;
            }
        }

        if(cursor == Cursor::None)
            break;
    }

    if(root) {
        if(!bounds.isValid())
            bounds = root->getSourceRange();
        ControlFlowCollector(bounds, result, source_manager)
            .TraverseStmt(const_cast<clang::Stmt*>(root));
    }
    return result;
}

std::optional<DocumentHighlight> to_highlight(CompilationUnitRef unit,
                                              clang::SourceLocation location) {
    location = unit.file_location(location);
    if(!location.isValid())
        return std::nullopt;

    const auto* token = unit.token_buffer().spelledTokenContaining(location);
    if(!token)
        return std::nullopt;

    auto begin = unit.file_location(token->location());
    auto end = unit.file_location(token->endLocation());
    if(!begin.isValid() || !end.isValid())
        return std::nullopt;

    auto interested = unit.interested_file();
    if(unit.file_id(begin) != interested || unit.file_id(end) != interested)
        return std::nullopt;

    return DocumentHighlight{
        .range = LocalSourceRange{unit.file_offset(begin), unit.file_offset(end)},
        .kind = protocol::DocumentHighlightKind::Text,
    };
}

}  // namespace

auto document_highlights(CompilationUnitRef unit, std::uint32_t offset)
    -> std::vector<DocumentHighlight> {
    if(offset > unit.interested_content().size())
        return {};

    std::vector<DocumentHighlight> result;
    SelectionTree::create_each(unit, LocalSourceRange{offset, offset}, [&](SelectionTree tree) {
        const auto* node = tree.common_ancestor();
        if(!node)
            return false;

        auto locations = related_control_flow(*node);
        if(locations.empty())
            return false;

        for(auto location: locations) {
            if(auto highlight = to_highlight(unit, location)) {
                result.push_back(*highlight);
            }
        }
        return !result.empty();
    });
    return result;
}

auto document_highlights(CompilationUnitRef unit, std::uint32_t offset, PositionEncoding encoding)
    -> std::vector<protocol::DocumentHighlight> {
    auto internal = document_highlights(unit, offset);
    PositionMapper mapper(unit.interested_content(), encoding);
    std::vector<protocol::DocumentHighlight> result;
    result.reserve(internal.size());

    for(const auto& highlight: internal) {
        result.push_back(protocol::DocumentHighlight{
            .range = to_range(mapper, highlight.range),
            .kind = highlight.kind,
        });
    }
    return result;
}

}  // namespace clice::feature

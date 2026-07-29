#include "semantic/semantics.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <ranges>
#include <set>
#include <utility>
#include <vector>

#include "compile/compilation_unit.h"
#include "semantic/ast_utility.h"
#include "semantic/resolver.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "clang/AST/Attr.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/AST/Expr.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/ExprConcepts.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/AST/Stmt.h"
#include "clang/Basic/OperatorKinds.h"
#include "clang/Basic/TokenKinds.h"
#include "clang/Lex/Lexer.h"
#include "clang/Tooling/Syntax/Tokens.h"

namespace clice {

SemanticNode::Kind SemanticNode::kind() const {
    if(auto macro = std::get_if<const MacroRef*>(&storage)) {
        switch((*macro)->kind) {
            case MacroRef::Kind::Def: return Kind::MacroDefine;
            case MacroRef::Kind::Ref: return Kind::MacroReference;
            case MacroRef::Kind::Undef: return Kind::MacroUndef;
        }
    }

    /// The variant alternatives before the macro slot map 1:1 onto Kind; the
    /// slots after it are shifted by the two extra macro kinds.
    constexpr std::size_t macro_slot = 9;
    auto index = storage.index();
    if(index < macro_slot) {
        return static_cast<Kind>(index);
    }
    return static_cast<Kind>(index + 2);
}

clang::SourceRange SemanticNode::source_range() const {
    return visit([](const auto& value) -> clang::SourceRange {
        using T = std::remove_cvref_t<decltype(value)>;
        if constexpr(std::same_as<T, const MacroRef*>) {
            return clang::SourceRange(value->loc);
        } else if constexpr(std::same_as<T, const Include*>) {
            return clang::SourceRange(value->location);
        } else if constexpr(std::same_as<T, const Import*>) {
            if(!value->name_locations.empty()) {
                return clang::SourceRange(value->location, value->name_locations.back());
            }
            return clang::SourceRange(value->location);
        } else if constexpr(std::same_as<T, const clang::Attr*>) {
            return value->getRange();
        } else if constexpr(std::is_pointer_v<T>) {
            return value->getSourceRange();
        } else {
            return value.getSourceRange();
        }
    });
}

clang::DynTypedNode SemanticNode::dyn_typed() const {
    assert(is_ast() && "dyn_typed is only valid for AST nodes");
    return visit([](const auto& value) -> clang::DynTypedNode {
        using T = std::remove_cvref_t<decltype(value)>;
        if constexpr(std::same_as<T, const MacroRef*> || std::same_as<T, const Include*> ||
                     std::same_as<T, const Import*>) {
            llvm_unreachable("dyn_typed on a preprocessor node");
        } else if constexpr(std::is_pointer_v<T>) {
            return clang::DynTypedNode::create(*value);
        } else {
            return clang::DynTypedNode::create(value);
        }
    });
}

bool should_ignore_token(const clang::syntax::Token& token) {
    switch(token.kind()) {
        // Even "attached" comments are not considered part of a node's range.
        case clang::tok::comment:
        // The AST doesn't directly store locations for terminating semicolons.
        case clang::tok::semi:
        // We don't have locations for cvr-qualifiers: see QualifiedTypeLoc.
        case clang::tok::kw_const:
        case clang::tok::kw_volatile:
        case clang::tok::kw_restrict: return true;
        default: return false;
    }
}

namespace {

// An IntervalSet maintains a set of disjoint subranges of an array.
//
// Initially, it contains the entire array.
//           [-----------------------------------------------------------]
//
// When a range is erased(), it will typically split the array in two.
//  claim:                     [--------------------]
//  after:   [----------------]                      [-------------------]
//
// erase() returns the segments actually erased. Given the state above:
//  claim:          [---------------------------------------]
//  out:            [---------]                      [------]
//  after:   [-----]                                         [-----------]
//
// It is used to track (expanded) tokens not yet associated with an AST node.
// On traversing an AST node, its token range is erased from the unclaimed set.
// The tokens actually removed are owned by that node.
class IntervalSet {
public:
    using Token = clang::syntax::Token;
    using TokenRange = llvm::ArrayRef<Token>;

    IntervalSet(TokenRange range) {
        unclaimed_ranges.insert(range);
    }

    // Removes the elements of Claim from the set, modifying or removing ranges
    // that overlap it.
    // Returns the continuous subranges of Claim that were actually removed.
    llvm::SmallVector<TokenRange> erase(TokenRange claim) {
        llvm::SmallVector<TokenRange> out;
        if(claim.empty())
            return out;

        // General case:
        // Claim:                   [-----------------]
        // unclaimed_ranges: [-A-] [-B-] [-C-] [-D-] [-E-] [-F-] [-G-]
        // Overlap:               ^first                  ^second
        // Ranges C and D are fully included. Ranges B and E must be trimmed.
        auto overlap =
            std::make_pair(unclaimed_ranges.lower_bound({claim.begin(), claim.begin()}),  // C
                           unclaimed_ranges.lower_bound({claim.end(), claim.end()}));     // F
        // Rewind to cover B.
        if(overlap.first != unclaimed_ranges.begin()) {
            --overlap.first;
            // ...unless B isn't selected at all.
            if(overlap.first->end() <= claim.begin()) {
                ++overlap.first;
            }
        }

        if(overlap.first == overlap.second) {
            return out;
        }

        // First, copy all overlapping ranges into the output.
        auto out_first = out.insert(out.end(), overlap.first, overlap.second);
        // If any of the overlapping ranges were sliced by the claim, split them:
        //  - restrict the returned range to the claimed part
        //  - save the unclaimed part so it can be reinserted
        TokenRange remaining_head, remaining_tail;
        if(claim.begin() > out_first->begin()) {
            remaining_head = {out_first->begin(), claim.begin()};
            *out_first = {claim.begin(), out_first->end()};
        }
        if(claim.end() < out.back().end()) {
            remaining_tail = {claim.end(), out.back().end()};
            out.back() = {out.back().begin(), claim.end()};
        }

        // Erase all the overlapping ranges (invalidating all iterators).
        unclaimed_ranges.erase(overlap.first, overlap.second);
        // Reinsert ranges that were merely trimmed.
        if(!remaining_head.empty()) {
            unclaimed_ranges.insert(remaining_head);
        }
        if(!remaining_tail.empty()) {
            unclaimed_ranges.insert(remaining_tail);
        }

        return out;
    }

private:
    struct range_less {
        bool operator()(TokenRange L, TokenRange R) const {
            return L.begin() < R.begin();
        }
    };

    // Disjoint sorted unclaimed ranges of expanded tokens.
    std::set<TokenRange, range_less> unclaimed_ranges;
};

// Determine whether 'Target' is the first expansion of the macro
// argument whose top-level spelling location is 'SpellingLoc'.
bool is_first_expansion(clang::FileID target,
                        clang::SourceLocation spelling_loc,
                        const clang::SourceManager& source_manager) {
    clang::SourceLocation prev = spelling_loc;
    while(true) {
        // If the arg is expanded multiple times, getMacroArgExpandedLocation()
        // returns the first expansion.
        clang::SourceLocation next = source_manager.getMacroArgExpandedLocation(prev);
        // So if we reach the target, target is the first-expansion of the
        // first-expansion ...
        if(source_manager.getFileID(next) == target) {
            return true;
        }

        // Otherwise, if the FileID stops changing, we've reached the innermost
        // macro expansion, and Target was on a different branch.
        if(source_manager.getFileID(next) == source_manager.getFileID(prev)) {
            return false;
        }

        prev = next;
    }
    return false;
}

bool is_implicit(const clang::Stmt* statement) {
    // Some Stmts are implicit and shouldn't be traversed, but there's no
    // "implicit" attribute on Stmt/Expr.
    // Unwrap implicit casts first if present (other nodes too?).
    if(auto* ICE = llvm::dyn_cast<clang::ImplicitCastExpr>(statement)) {
        statement = ICE->getSubExprAsWritten();
    }

    // Implicit this in a MemberExpr is not filtered out by RecursiveASTVisitor.
    // It would be nice if RAV handled this (!shouldTraverseImplicitCode()).
    if(auto* CTI = llvm::dyn_cast<clang::CXXThisExpr>(statement)) {
        if(CTI->isImplicit()) {
            return true;
        }
    }

    // Make sure implicit access of anonymous structs don't end up owning tokens.
    if(auto* ME = llvm::dyn_cast<clang::MemberExpr>(statement)) {
        if(auto* FD = llvm::dyn_cast<clang::FieldDecl>(ME->getMemberDecl())) {
            if(FD->isAnonymousStructOrUnion()) {
                // If Base is an implicit CXXThis, then the whole MemberExpr has no
                // tokens. If it's a normal e.g. DeclRef, we treat the MemberExpr like
                // an implicit cast.
                return is_implicit(ME->getBase());
            }
        }
    }

    // Refs to operator() and [] are (almost?) always implicit as part of calls.
    if(auto* DRE = llvm::dyn_cast<clang::DeclRefExpr>(statement)) {
        if(auto* FD = llvm::dyn_cast<clang::FunctionDecl>(DRE->getDecl())) {
            switch(FD->getOverloadedOperator()) {
                case clang::OO_Call:
                case clang::OO_Subscript: return true;
                default: break;
            }
        }
    }

    return false;
}

// The range whose tokens a node claims. Usually just the source range, with
// one exception: MemberExprs to implicitly access anonymous fields should not
// claim any tokens for themselves. Given:
//   struct A { struct { int b; }; };
// The clang AST reports the following nodes for an access to b:
//   A().b;
//   [----] MemberExpr, base = A().<anonymous>, member = b
//   [----] MemberExpr: base = A(), member = <anonymous>
//   [-]    CXXConstructExpr
// For our purposes, we don't want the second MemberExpr to own any tokens,
// so we reduce its range to match the CXXConstructExpr.
clang::SourceRange claimed_source_range(const SemanticNode& node) {
    if(const auto* ME = node.get<clang::MemberExpr>()) {
        if(!ME->getMemberDecl()->getDeclName()) {
            if(auto* base = ME->getBase()) {
                return claimed_source_range(SemanticNode(static_cast<const clang::Stmt*>(base)));
            }
            return clang::SourceRange();
        }
    }
    return node.source_range();
}

}  // namespace

// Builds the Semantics of the interested file: one full traversal of its AST,
// claiming expanded tokens innermost-first, then attributing every claimed
// expanded token to the spelled main-file token it came from:
//   - tokens written in the main file attribute to themselves
//   - tokens from an #included file attribute to the include's filename token
//   - macro-argument tokens (first expansion) attribute to the argument tokens
//   - other macro tokens attribute to the macro name at the expansion point
// Finally the preprocessor directives (macros, includes, imports) are appended
// as nodes of their own, owning their name tokens.
//
// The traversal maintains a parent stack. Nodes claim their tokens when they
// pop, so children get to claim parts of their range first, and parents only
// own tokens that no child owned.
//
// Nodes *usually* nest nicely: a child's getSourceRange() lies within the
// parent's, and a node (transitively) owns all tokens in its range.
//
// Exception 1: when declarators nest, *inner* declarator is the *outer* type.
//              e.g. void foo[5](int) is an array of functions.
// To handle this case, declarators are careful to only claim the tokens they
// own, rather than claim a range and rely on claim ordering.
//
// Exception 2: siblings both claim the same node.
//              e.g. `int x, y;` produces two sibling VarDecls.
//                    ~~~~~ x
//                    ~~~~~~~~ y
// Here the first ("leftmost") sibling claims the tokens it wants, and the
// other sibling gets what's left. So selecting "int" only includes the left
// VarDecl in the selection tree.
class SemanticsBuilder : public clang::RecursiveASTVisitor<SemanticsBuilder> {
public:
    static void build(Semantics& semantics, CompilationUnitRef unit, bool interested_only) {
        SemanticsBuilder builder(semantics, unit, interested_only);
        builder.TraverseAST(unit.context());
        assert(builder.stack.size() == 1 && "Unpaired push/pop?");
        builder.append_directives();
        builder.finalize();
    }

    // We traverse all "well-behaved" nodes the same way:
    //  - push the node onto the stack
    //  - traverse its children recursively
    //  - pop it from the stack, claiming its unclaimed tokens
    //
    // Two categories of nodes are not "well-behaved":
    //  - those without source range information, we don't record those
    //  - those that can't be stored in SemanticNode.
    bool TraverseDecl(clang::Decl* X) {
        if(llvm::isa_and_nonnull<clang::TranslationUnitDecl>(X)) {
            /// The TU decl is not stored; its children become roots.
            if(interested_only) {
                for(auto decl: unit.top_level_decls()) {
                    if(!TraverseDecl(decl)) {
                        return false;
                    }
                }
                return true;
            }

            return Base::TraverseDecl(X);
        }

        // Base::TraverseDecl will suppress children, but not this node itself.
        if(X && X->isImplicit()) {
            // Most implicit nodes have only implicit children and can be skipped.
            // However there are exceptions (`void foo(Concept auto x)`), and
            // the base implementation knows how to find them.
            return Base::TraverseDecl(X);
        }

        if(!X) {
            return true;
        }

        return traverse_node(SemanticNode(static_cast<const clang::Decl*>(X)),
                             [&] { return Base::TraverseDecl(X); });
    }

    bool TraverseTypeLoc(clang::TypeLoc X) {
        if(!X) {
            return true;
        }

        return traverse_node(SemanticNode(X), [&] { return Base::TraverseTypeLoc(X); });
    }

    bool TraverseTemplateArgumentLoc(const clang::TemplateArgumentLoc& X) {
        return traverse_node(SemanticNode(X), [&] { return Base::TraverseTemplateArgumentLoc(X); });
    }

    bool TraverseNestedNameSpecifierLoc(clang::NestedNameSpecifierLoc X) {
        if(!X) {
            return true;
        }

        return traverse_node(SemanticNode(X),
                             [&] { return Base::TraverseNestedNameSpecifierLoc(X); });
    }

    bool TraverseConstructorInitializer(clang::CXXCtorInitializer* X) {
        if(!X) {
            return true;
        }

        return traverse_node(SemanticNode(static_cast<const clang::CXXCtorInitializer*>(X)),
                             [&] { return Base::TraverseConstructorInitializer(X); });
    }

    bool TraverseCXXBaseSpecifier(const clang::CXXBaseSpecifier& X) {
        return traverse_node(SemanticNode(&X), [&] { return Base::TraverseCXXBaseSpecifier(X); });
    }

    bool TraverseAttr(clang::Attr* X) {
        if(!X) {
            return true;
        }

        return traverse_node(SemanticNode(static_cast<const clang::Attr*>(X)),
                             [&] { return Base::TraverseAttr(X); });
    }

    bool TraverseConceptReference(clang::ConceptReference* X) {
        if(!X) {
            return true;
        }

        return traverse_node(SemanticNode(static_cast<const clang::ConceptReference*>(X)),
                             [&] { return Base::TraverseConceptReference(X); });
    }

    // Stmt is the same, but this form allows the data recursion optimization.
    bool dataTraverseStmtPre(clang::Stmt* X) {
        if(!X || is_implicit(X)) {
            return false;
        }

        push(SemanticNode(static_cast<const clang::Stmt*>(X)));
        if(should_skip_children(X)) {
            pop();
            return false;
        }

        return true;
    }

    bool dataTraverseStmtPost(clang::Stmt* X) {
        pop();
        return true;
    }

    // QualifiedTypeLoc is handled strangely in RecursiveASTVisitor: the derived
    // TraverseTypeLoc is not called for the inner UnqualTypeLoc.
    // This means we'd never see 'int' in 'const int'! Work around that here.
    // (The reason for the behavior is to avoid traversing the nested Type twice,
    // but we ignore TraverseType anyway).
    bool TraverseQualifiedTypeLoc(clang::QualifiedTypeLoc QX) {
        return traverse_node(SemanticNode(static_cast<clang::TypeLoc>(QX)),
                             [&] { return TraverseTypeLoc(QX.getUnqualifiedLoc()); });
    }

    bool TraverseType(clang::QualType) {
        return true;
    }

    // Uninteresting parts of the AST that don't have locations within them.
    bool TraverseNestedNameSpecifier(clang::NestedNameSpecifier*) {
        return true;
    }

    // The DeclStmt for the loop variable claims to cover the whole range
    // inside the parens, this causes the range-init expression to not be hit.
    // Traverse the loop VarDecl instead, which has the right source range.
    bool TraverseCXXForRangeStmt(clang::CXXForRangeStmt* S) {
        return traverse_node(SemanticNode(static_cast<const clang::Stmt*>(S)), [&] {
            return TraverseStmt(S->getInit()) && TraverseDecl(S->getLoopVariable()) &&
                   TraverseStmt(S->getRangeInit()) && TraverseStmt(S->getBody());
        });
    }

    // OpaqueValueExpr blocks traversal, we must explicitly traverse it.
    bool TraverseOpaqueValueExpr(clang::OpaqueValueExpr* E) {
        return traverse_node(SemanticNode(static_cast<const clang::Stmt*>(E)),
                             [&] { return TraverseStmt(E->getSourceExpr()); });
    }

    // We only want to traverse the *syntactic form* to understand the selection.
    bool TraversePseudoObjectExpr(clang::PseudoObjectExpr* E) {
        return traverse_node(SemanticNode(static_cast<const clang::Stmt*>(E)),
                             [&] { return TraverseStmt(E->getSyntacticForm()); });
    }

    bool TraverseTypeConstraint(const clang::TypeConstraint* C) {
        if(auto* E = C->getImmediatelyDeclaredConstraint()) {
            // Technically this expression is 'implicit' and not traversed by the RAV.
            // However, the range is correct, so we visit expression to avoid adding
            // an extra kind to 'SemanticNode' that hold 'TypeConstraint'.
            return TraverseStmt(E);
        }
        return Base::TraverseTypeConstraint(C);
    }

    // Override child traversal for certain node types.
    using RecursiveASTVisitor::getStmtChildren;

    // PredefinedExpr like __func__ has a StringLiteral child for its value.
    // It's not written, so don't traverse it.
    clang::Stmt::child_range getStmtChildren(clang::PredefinedExpr*) {
        return {clang::StmtIterator{}, clang::StmtIterator{}};
    }

private:
    using Base = RecursiveASTVisitor<SemanticsBuilder>;

    SemanticsBuilder(Semantics& semantics, CompilationUnitRef unit, bool interested_only) :
        semantics(semantics), unit(unit), interested_only(interested_only),
        SM(unit.context().getSourceManager()), unclaimed_expanded_tokens(unit.expanded_tokens()) {
        main_fid = unit.interested_file();
        main_file_range =
            clang::SourceRange(SM.getLocForStartOfFile(main_fid), SM.getLocForEndOfFile(main_fid));

        semantics.tokens = unit.spelled_tokens(main_fid);
        semantics.file_begin = main_file_range.getBegin();
        semantics.pp_ignored.resize(semantics.tokens.size(), false);

        // Tokens preprocessed to nothing (e.g. a disabled region or an empty
        // macro invocation) never contribute to a selection. Only relevant
        // when token ownership is recorded at all.
        if(interested_only) {
            for(const clang::syntax::TokenBuffer::Expansion& expansion:
                unit.expansions_overlapping(semantics.tokens)) {
                if(expansion.Expanded.empty()) {
                    for(const clang::syntax::Token& token: expansion.Spelled) {
                        std::size_t i = &token - semantics.tokens.data();
                        if(i < semantics.pp_ignored.size()) {
                            semantics.pp_ignored[i] = true;
                        }
                    }
                }
            }
        }

        stack.push_back(Semantics::invalid);
    }

    template <typename Func>
    bool traverse_node(SemanticNode node, const Func& body) {
        push(std::move(node));
        bool ret = body();
        pop();
        return ret;
    }

    // There are certain nodes we want to treat as leaves,
    // although they do have children.
    bool should_skip_children(const clang::Stmt* X) const {
        // UserDefinedLiteral (e.g. 12_i) has two children (12 and _i).
        // Unfortunately TokenBuffer sees 12_i as one token and can't split it.
        // So we treat UserDefinedLiteral as a leaf node, owning the token.
        return llvm::isa<clang::UserDefinedLiteral>(X);
    }

    // Pushes a node onto the ancestor stack. Pairs with pop().
    // Performs early claiming for some nodes (on the earlySourceRange).
    void push(SemanticNode node) {
        clang::SourceRange early = early_source_range(node);
        auto self = static_cast<std::uint32_t>(semantics.nodes.size());
        semantics.nodes.push_back({std::move(node), stack.back()});
        stack.push_back(self);
        claim_range(early, self);
    }

    // Pops a node off the ancestor stack, claims its tokens and seals its
    // subtree extent.
    void pop() {
        std::uint32_t self = stack.back();
        claim_tokens_for(semantics.nodes[self].node, self);
        semantics.nodes[self].subtree_end = static_cast<std::uint32_t>(semantics.nodes.size());
        stack.pop_back();
    }

    // Returns the range of tokens that this node will claim directly, and
    // is not available to the node's children.
    // Usually empty, but sometimes children cover tokens but shouldn't own them.
    clang::SourceRange early_source_range(const SemanticNode& N) {
        if(const clang::Decl* VD = N.get<clang::VarDecl>()) {
            // We want the name in the var-decl to be claimed by the decl itself and
            // not by any children. Ususally, we don't need this, because source
            // ranges of children are not overlapped with their parent's.
            // An exception is lambda captured var decl, where AutoTypeLoc is
            // overlapped with the name loc.
            //    auto fun = [bar = foo]() { ... }
            //                ~~~~~~~~~   VarDecl
            //                ~~~         |- AutoTypeLoc
            return VD->getLocation();
        }

        // When referring to a destructor ~Foo(), attribute Foo to the destructor
        // rather than the TypeLoc nested inside it.
        // We still traverse the TypeLoc, because it may contain other targeted
        // things like the T in ~Foo<T>().
        if(const auto* CDD = N.get<clang::CXXDestructorDecl>()) {
            return CDD->getNameInfo().getNamedTypeInfo()->getTypeLoc().getBeginLoc();
        }

        if(const auto* ME = N.get<clang::MemberExpr>()) {
            auto name_info = ME->getMemberNameInfo();
            if(name_info.getName().getNameKind() == clang::DeclarationName::CXXDestructorName) {
                return name_info.getNamedTypeInfo()->getTypeLoc().getBeginLoc();
            }
        }

        return clang::SourceRange();
    }

    // Claim tokens for N, after processing its children.
    // By default this claims all unclaimed tokens in its source range.
    // We override this if we want to claim fewer tokens (e.g. there are gaps).
    void claim_tokens_for(const SemanticNode& N, std::uint32_t self) {
        // CXXConstructExpr often shows implicit construction, like `string s;`.
        // Don't associate any tokens with it unless there's some syntax like {}.
        // This prevents it from claiming 's', its primary location.
        if(const auto* CCE = N.get<clang::CXXConstructExpr>()) {
            claim_range(CCE->getParenOrBraceRange(), self);
            return;
        }

        // ExprWithCleanups is always implicit. It often wraps CXXConstructExpr.
        // Prevent it claiming 's' in the case above.
        if(N.get<clang::ExprWithCleanups>()) {
            return;
        }

        // Declarators nest "inside out", with parent types inside child ones.
        // Instead of claiming the whole range (clobbering parent tokens), carefully
        // claim the tokens owned by this node and non-declarator children.
        // (We could manipulate traversal order instead, but this is easier).
        //
        // Non-declarator types nest normally, and are handled like other nodes.
        //
        // Example:
        //   Vec<R<int>(*[2])(A<char>)> is a Vec of arrays of pointers to functions,
        //                              which accept A<char> and return R<int>.
        // The TypeLoc hierarchy:
        //   Vec<R<int>(*[2])(A<char>)> m;
        //   Vec<#####################>      TemplateSpecialization Vec
        //       --------[2]----------       `-Array
        //       -------*-------------         `-Pointer
        //       ------(----)---------           `-Paren
        //       ------------(#######)             `-Function
        //       R<###>                              |-TemplateSpecialization R
        //         int                               | `-Builtin int
        //                    A<####>                `-TemplateSpecialization A
        //                      char                   `-Builtin char
        //
        // In each row
        //   --- represents unclaimed parts of the SourceRange.
        //   ### represents parts that children already claimed.
        if(const auto* TL = N.get<clang::TypeLoc>()) {
            if(auto PTL = TL->getAs<clang::ParenTypeLoc>()) {
                claim_range(PTL.getLParenLoc(), self);
                claim_range(PTL.getRParenLoc(), self);
                return;
            }

            if(auto ATL = TL->getAs<clang::ArrayTypeLoc>()) {
                claim_range(ATL.getBracketsRange(), self);
                return;
            }

            if(auto PTL = TL->getAs<clang::PointerTypeLoc>()) {
                claim_range(PTL.getStarLoc(), self);
                return;
            }

            if(auto FTL = TL->getAs<clang::FunctionTypeLoc>()) {
                claim_range(clang::SourceRange(FTL.getLParenLoc(), FTL.getEndLoc()), self);
                return;
            }
        }

        claim_range(claimed_source_range(N), self);
    }

    // Claim all unclaimed expanded tokens in S for node `self`, and attribute
    // each claimed token back to the main file.
    //
    // The whole-TU shape only feeds the index projection, which never queries
    // token ownership — skip the claim machinery entirely. This matters: the
    // preamble-state build runs a whole-TU pass over the preamble unit, and
    // claiming its entire expanded stream would be paid on every PCH build.
    void claim_range(clang::SourceRange S, std::uint32_t self) {
        if(!interested_only) {
            return;
        }

        for(const auto& claimed: unclaimed_expanded_tokens.erase(unit.expanded_tokens(S))) {
            attribute_tokens(claimed, self);
        }
    }

    // Attribute a consecutive range of claimed expanded tokens to spelled
    // main-file tokens: main-file tokens map to themselves, #included files
    // map to the include, macro args map to the spelled argument (first
    // expansion only), and other macro tokens map to the macro name.
    void attribute_tokens(llvm::ArrayRef<clang::syntax::Token> expanded_tokens,
                          std::uint32_t self) {
        if(expanded_tokens.empty()) {
            return;
        }

        // The eof token is used as a sentinel.
        // In general, source range from an AST node should not claim the eof token,
        // but it could occur for unmatched-bracket cases.
        if(expanded_tokens.back().kind() == clang::tok::eof) {
            expanded_tokens = expanded_tokens.drop_back();
        }

        while(!expanded_tokens.empty()) {
            // Take consecutive tokens from the same context together for efficiency.
            clang::SourceLocation start = expanded_tokens.front().location();
            clang::FileID fid = SM.getFileID(start);
            // Comparing SourceLocations against bounds is cheaper than getFileID().
            clang::SourceLocation limit = SM.getComposedLoc(fid, SM.getFileIDSize(fid));
            auto batch = expanded_tokens.take_while([&](const clang::syntax::Token& T) {
                return T.location() >= start && T.location() < limit;
            });
            assert(!batch.empty());
            expanded_tokens = expanded_tokens.drop_front(batch.size());

            attribute_batch(fid, batch, self);
        }
    }

    // Attribute a consecutive range of tokens from a single file ID.
    void attribute_batch(clang::FileID fid,
                         llvm::ArrayRef<clang::syntax::Token> batch,
                         std::uint32_t self) {
        assert(!batch.empty());
        clang::SourceLocation start_loc = batch.front().location();

        // Handle tokens written directly in the main file.
        if(fid == main_fid) {
            add_range_entries(*offset_in_main_file(batch.front().location()),
                              *offset_in_main_file(batch.back().location()),
                              self);
            return;
        }

        // Handle tokens in another file #included into the main file.
        // Attribute them to the #include's filename token, non-exclusively.
        if(start_loc.isFileID()) {
            for(clang::SourceLocation loc = batch.front().location(); loc.isValid();
                loc = SM.getIncludeLoc(SM.getFileID(loc))) {
                if(auto offset = offset_in_main_file(loc)) {
                    // FIXME: use whole #include directive, not just the filename string.
                    add_token_entry(*offset, self);
                    return;
                }
            }
            return;
        }

        assert(start_loc.isMacroID());
        // Handle tokens that were passed as a macro argument.
        clang::SourceLocation arg_start = SM.getTopMacroCallerLoc(start_loc);
        if(auto arg_offset = offset_in_main_file(arg_start)) {
            if(is_first_expansion(fid, arg_start, SM)) {
                clang::SourceLocation arg_end = SM.getTopMacroCallerLoc(batch.back().location());
                if(auto arg_end_offset = offset_in_main_file(arg_end)) {
                    add_range_entries(*arg_offset, *arg_end_offset, self);
                    return;
                }
            }
            /* else: fall through and treat as part of the macro body */
        }

        // Handle tokens produced by non-argument macro expansion.
        // Attribute them to the macro name, non-exclusively.
        if(auto expansion_offset = offset_in_main_file(get_expansion_start(start_loc))) {
            // FIXME: also check ( and ) for function-like macros?
            add_token_entry(*expansion_offset, self);
        }
    }

    // The index of the first spelled token with offset >= `offset`.
    std::uint32_t first_token_at(unsigned offset) const {
        auto count = static_cast<std::uint32_t>(semantics.tokens.size());
        auto range = std::views::iota(0u, count);
        auto it = std::ranges::partition_point(range, [&](std::uint32_t i) {
            return semantics.token_offset(i) < offset;
        });
        return static_cast<std::uint32_t>(it - range.begin());
    }

    // Add ownership entries for every countable spelled token whose offset
    // lies in the closed range [begin, end].
    void add_range_entries(unsigned begin, unsigned end, std::uint32_t self) {
        assert(begin <= end);
        auto count = static_cast<std::uint32_t>(semantics.tokens.size());
        for(auto i = first_token_at(begin); i < count && semantics.token_offset(i) <= end; i++) {
            if(!should_ignore_token(semantics.tokens[i]) && !semantics.pp_ignored[i]) {
                entries.emplace_back(i, self);
            }
        }
    }

    // Add one ownership entry for the spelled token exactly at `offset`, if any.
    void add_token_entry(unsigned offset, std::uint32_t self) {
        auto i = first_token_at(offset);
        if(i < semantics.tokens.size() && semantics.token_offset(i) == offset) {
            if(!should_ignore_token(semantics.tokens[i]) && !semantics.pp_ignored[i]) {
                entries.emplace_back(i, self);
            }
        }
    }

    // Add one ownership entry at `offset` regardless of the selection ignore
    // set — preprocessor nodes own their directive tokens.
    void add_directive_entry(unsigned offset, std::uint32_t self) {
        auto i = first_token_at(offset);
        if(i < semantics.tokens.size() && semantics.token_offset(i) == offset) {
            entries.emplace_back(i, self);
        }
    }

    // Decomposes Loc and returns the offset if the file ID is the main file.
    std::optional<unsigned> offset_in_main_file(clang::SourceLocation location) const {
        // Decoding Loc with SM.getDecomposedLoc is relatively expensive.
        // But SourceLocations for a file are numerically contiguous, so we
        // can use cheap integer operations instead.
        if(location < main_file_range.getBegin() || location >= main_file_range.getEnd()) {
            return std::nullopt;
        }

        return location.getRawEncoding() - main_file_range.getBegin().getRawEncoding();
    }

    clang::SourceLocation get_expansion_start(clang::SourceLocation location) const {
        while(location.isMacroID()) {
            location = SM.getImmediateExpansionRange(location).getBegin();
        }
        return location;
    }

    // Append the interested file's preprocessor directives as nodes owning
    // their name tokens. These live outside the AST segment: parent chains do
    // not include them (yet), and selection ignores them, but hover and
    // document links see macros, includes and imports as first-class nodes.
    void append_directives() {
        auto it = unit.directives().find(main_fid);
        if(it == unit.directives().end()) {
            return;
        }

        struct Row {
            unsigned offset;
            SemanticNode node;
            llvm::SmallVector<unsigned, 2> extra_offsets;
        };

        std::vector<Row> rows;
        auto& directive = it->second;

        for(auto& macro: directive.macros) {
            if(auto offset = offset_in_main_file(macro.loc)) {
                rows.push_back({*offset, SemanticNode(&macro), {}});
            }
        }

        for(auto& include: directive.includes) {
            if(auto offset = offset_in_main_file(include.location)) {
                rows.push_back({*offset, SemanticNode(&include), {}});
            }
        }

        for(auto& import: directive.imports) {
            /// An import spelled through a macro carries macro locations;
            /// name tokens written as macro arguments still resolve to
            /// spelled main-file tokens.
            Row row{0, SemanticNode(&import), {}};
            std::optional<unsigned> anchor;
            if(auto offset = offset_in_main_file(import.location)) {
                anchor = *offset;
                row.extra_offsets.push_back(*offset);
            }
            for(auto loc: import.name_locations) {
                if(loc.isMacroID()) {
                    loc = SM.getSpellingLoc(loc);
                }
                if(auto name_offset = offset_in_main_file(loc)) {
                    if(!anchor) {
                        anchor = *name_offset;
                    }
                    row.extra_offsets.push_back(*name_offset);
                }
            }
            if(anchor) {
                row.offset = *anchor;
                rows.push_back(std::move(row));
            }
        }

        std::ranges::sort(rows, {}, &Row::offset);

        for(auto& row: rows) {
            auto self = static_cast<std::uint32_t>(semantics.nodes.size());
            semantics.nodes.push_back({row.node, Semantics::invalid, self + 1});
            add_directive_entry(row.offset, self);
            for(auto offset: row.extra_offsets) {
                add_directive_entry(offset, self);
            }
        }
    }

    // Sort the accumulated entries into the CSR layout and count per-node
    // ownership.
    void finalize() {
        std::ranges::sort(entries);
        auto duplicates = std::ranges::unique(entries);
        entries.erase(duplicates.begin(), duplicates.end());

        semantics.owner_begin.assign(semantics.tokens.size() + 1, 0);
        semantics.owner_nodes.reserve(entries.size());
        for(auto [token, node]: entries) {
            semantics.owner_begin[token + 1]++;
            semantics.owner_nodes.push_back(node);
            semantics.nodes[node].owned++;
        }
        for(std::size_t i = 1; i < semantics.owner_begin.size(); i++) {
            semantics.owner_begin[i] += semantics.owner_begin[i - 1];
        }
    }

    Semantics& semantics;
    CompilationUnitRef unit;
    bool interested_only;
    clang::SourceManager& SM;
    clang::FileID main_fid;
    clang::SourceRange main_file_range;
    IntervalSet unclaimed_expanded_tokens;
    llvm::SmallVector<std::uint32_t, 64> stack;

    /// (spelled token index, owning node index) pairs, sorted in finalize().
    std::vector<std::pair<std::uint32_t, std::uint32_t>> entries;
};

Semantics Semantics::build(CompilationUnitRef unit, bool interested_only) {
    Semantics semantics;
    SemanticsBuilder::build(semantics, unit, interested_only);
    return semantics;
}

namespace {

using Occurrences = llvm::SmallVector<NameOccurrence, 2>;

void occur(Occurrences& out,
           const clang::NamedDecl* decl,
           RelationKind kind,
           clang::SourceLocation location) {
    if(decl && location.isValid()) {
        out.push_back({decl, kind, location});
    }
}

/// The per-decl-kind extraction below is ported verbatim from the former
/// SemanticVisitor: the same decls, the same roles, the same name locations.
void decl_occurrences(const clang::Decl* D, Occurrences& out, TemplateResolver* resolver) {
    /// namespace Foo = Bar
    ///            ^     ^~~~ reference
    ///            ^~~~ definition
    if(auto* NAD = llvm::dyn_cast<clang::NamespaceAliasDecl>(D)) {
        occur(out, NAD, RelationKind::Definition, NAD->getLocation());
        occur(out, NAD->getNamespace(), RelationKind::Reference, NAD->getTargetNameLoc());
        return;
    }

    /// namespace Foo { }
    ///            ^~~~ definition
    if(auto* ND = llvm::dyn_cast<clang::NamespaceDecl>(D)) {
        occur(out, ND, RelationKind::Definition, ND->getLocation());
        return;
    }

    /// using namespace Foo
    ///                  ^~~~~~~ reference
    if(auto* UDD = llvm::dyn_cast<clang::UsingDirectiveDecl>(D)) {
        /// As-written: `using namespace Alias` references the alias, not the
        /// underlying namespace.
        occur(out,
              UDD->getNominatedNamespaceAsWritten(),
              RelationKind::Reference,
              UDD->getLocation());
        return;
    }

    /// label:
    ///   ^~~~ definition
    if(auto* LD = llvm::dyn_cast<clang::LabelDecl>(D)) {
        occur(out, LD, RelationKind::Definition, LD->getLocation());
        return;
    }

    /// struct X { int foo; };
    ///                 ^~~~ definition
    if(auto* FD = llvm::dyn_cast<clang::FieldDecl>(D)) {
        occur(out, FD, RelationKind::Definition, FD->getLocation());
        return;
    }

    /// enum Foo { bar };
    ///             ^~~~ definition
    if(auto* ECD = llvm::dyn_cast<clang::EnumConstantDecl>(D)) {
        occur(out, ECD, RelationKind::Definition, ECD->getLocation());
        return;
    }

    /// using Base<T>::foo; — the dependent flavor of a using declaration.
    if(auto* UUV = llvm::dyn_cast<clang::UnresolvedUsingValueDecl>(D)) {
        if(resolver) {
            for(auto* target: resolver->lookup(UUV)) {
                occur(out, target, RelationKind::WeakReference, UUV->getLocation());
            }
        }
        return;
    }

    if(auto* UUT = llvm::dyn_cast<clang::UnresolvedUsingTypenameDecl>(D)) {
        if(resolver) {
            for(auto* target: resolver->lookup(UUT)) {
                occur(out, target, RelationKind::WeakReference, UUT->getLocation());
            }
        }
        return;
    }

    /// using Foo::bar;
    ///             ^~~~ reference
    /// Shadows are synthetic; reference their underlying targets so hover
    /// and the index see the real declarations.
    if(auto* UD = llvm::dyn_cast<clang::UsingDecl>(D)) {
        for(auto* shadow: UD->shadows()) {
            occur(out, shadow->getTargetDecl(), RelationKind::WeakReference, UD->getLocation());
        }
        return;
    }

    /// auto [a, b] = std::make_tuple(1, 2);
    ///       ^~~~ definition
    ///
    /// template <typename T> / template <int N>
    ///                    ^~~~ definition
    if(llvm::isa<clang::BindingDecl,
                 clang::TemplateTypeParmDecl,
                 clang::TemplateTemplateParmDecl,
                 clang::NonTypeTemplateParmDecl>(D)) {
        auto* ND = llvm::cast<clang::NamedDecl>(D);
        occur(out, ND, RelationKind::Definition, ND->getLocation());
        return;
    }

    /// template <typename T> concept Foo = ...;
    ///                                ^~~~ definition
    if(auto* CD = llvm::dyn_cast<clang::ConceptDecl>(D)) {
        occur(out, CD, RelationKind::Definition, CD->getLocation());
        return;
    }

    /// The template decl itself produces no occurrence; the templated decl
    /// inside it does.
    if(llvm::isa<clang::ClassTemplateDecl, clang::FunctionTemplateDecl, clang::VarTemplateDecl>(
           D)) {
        return;
    }

    /// struct/class/union/enum Foo { ... };
    ///                          ^~~~ declaration/definition
    if(auto* TD = llvm::dyn_cast<clang::TagDecl>(D)) {
        if(auto* CTSD = llvm::dyn_cast<clang::ClassTemplateSpecializationDecl>(TD)) {
            switch(CTSD->getSpecializationKind()) {
                /// Unlike the old filtered traversal, the semantic map may
                /// record implicit instantiations; they produce no written
                /// occurrence.
                case clang::TSK_Undeclared:
                case clang::TSK_ImplicitInstantiation: {
                    return;
                }

                case clang::TSK_ExplicitSpecialization: {
                    break;
                }

                case clang::TSK_ExplicitInstantiationDeclaration:
                case clang::TSK_ExplicitInstantiationDefinition: {
                    occur(out,
                          ast::instantiated_from(CTSD),
                          RelationKind::Reference,
                          CTSD->getLocation());
                    return;
                }
            }
        }

        RelationKind kind = TD->isThisDeclarationADefinition() ? RelationKind::Definition
                                                               : RelationKind::Declaration;
        occur(out, TD, kind, TD->getLocation());
        return;
    }

    /// void foo() { ... }
    ///       ^~~~ declaration/definition
    if(auto* FD = llvm::dyn_cast<clang::FunctionDecl>(D)) {
        switch(FD->getTemplateSpecializationKind()) {
            case clang::TSK_ImplicitInstantiation:
            /// FIXME: Clang currently doesn't record source location of explicit
            /// instantiation of function template correctly. Skip it temporarily.
            case clang::TSK_ExplicitInstantiationDeclaration:
            case clang::TSK_ExplicitInstantiationDefinition: {
                return;
            }

            case clang::TSK_Undeclared:
            case clang::TSK_ExplicitSpecialization: {
                break;
            }
        }

        RelationKind kind = FD->isThisDeclarationADefinition() ? RelationKind::Definition
                                                               : RelationKind::Declaration;
        occur(out, FD, kind, FD->getLocation());
        return;
    }

    /// using Foo = int;
    ///        ^~~~ definition
    if(auto* TND = llvm::dyn_cast<clang::TypedefNameDecl>(D)) {
        occur(out, TND, RelationKind::Definition, TND->getLocation());
        return;
    }

    /// int foo = 2;
    ///      ^~~~ declaration/definition
    if(auto* VD = llvm::dyn_cast<clang::VarDecl>(D)) {
        if(auto* VTSD = llvm::dyn_cast<clang::VarTemplateSpecializationDecl>(VD)) {
            switch(VTSD->getSpecializationKind()) {
                case clang::TSK_ImplicitInstantiation:
                /// FIXME: Clang currently doesn't record source location of explicit
                /// instantiation of variable template correctly. Skip it temporarily.
                case clang::TSK_ExplicitInstantiationDeclaration:
                case clang::TSK_ExplicitInstantiationDefinition: {
                    return;
                }

                case clang::TSK_Undeclared:
                case clang::TSK_ExplicitSpecialization: {
                    break;
                }
            }
        }

        RelationKind kind = VD->isThisDeclarationADefinition() ? RelationKind::Definition
                                                               : RelationKind::Declaration;
        occur(out, VD, kind, VD->getLocation());
        return;
    }
}

void type_loc_occurrences(clang::TypeLoc TL, Occurrences& out, TemplateResolver* resolver) {
    /// struct Foo foo;
    ///         ^~~~ reference
    if(auto TTL = TL.getAs<clang::TagTypeLoc>()) {
        occur(out, TTL.getDecl(), RelationKind::Reference, TTL.getNameLoc());
        return;
    }

    /// using Foo = int; Foo foo;
    ///                   ^~~~ reference
    if(auto TTL = TL.getAs<clang::TypedefTypeLoc>()) {
        occur(out, TTL.getTypedefNameDecl(), RelationKind::Reference, TTL.getNameLoc());
        return;
    }

    /// template <typename T> void foo(T t)
    ///                                ^~~~ reference
    if(auto TTPL = TL.getAs<clang::TemplateTypeParmTypeLoc>()) {
        occur(out, TTPL.getDecl(), RelationKind::Reference, TTPL.getNameLoc());
        return;
    }

    /// std::vector<int>
    ///        ^~~~ reference
    /// X *next; inside the definition of template<class T> struct X — the
    /// written X is the injected class name.
    if(auto ICNTL = TL.getAs<clang::InjectedClassNameTypeLoc>()) {
        occur(out, ICNTL.getDecl(), RelationKind::Reference, ICNTL.getNameLoc());
        return;
    }

    /// using typename B<T>::type; type x; — the dependent flavor keeps the
    /// unresolved-using declaration itself, resolved further when possible.
    if(auto UUTL = TL.getAs<clang::UnresolvedUsingTypeLoc>()) {
        auto* UD = UUTL.getTypePtr()->getDecl();
        occur(out, UD, RelationKind::WeakReference, UUTL.getNameLoc());
        if(resolver) {
            if(auto* UUT = llvm::dyn_cast<clang::UnresolvedUsingTypenameDecl>(UD)) {
                for(auto* target: resolver->lookup(UUT)) {
                    occur(out, target, RelationKind::WeakReference, UUTL.getNameLoc());
                }
            }
        }
        return;
    }

    /// using B::value_type; value_type x; — the written type resolves
    /// through the using shadow to the imported type.
    if(auto UTL = TL.getAs<clang::UsingTypeLoc>()) {
        occur(out,
              UTL.getTypePtr()->getFoundDecl()->getTargetDecl(),
              RelationKind::Reference,
              UTL.getNameLoc());
        return;
    }

    /// Box b(1); with CTAD — the written name is a deduced placeholder;
    /// reference the class template's pattern.
    if(auto DTSTL = TL.getAs<clang::DeducedTemplateSpecializationTypeLoc>()) {
        if(auto* TD = DTSTL.getTypePtr()->getTemplateName().getAsTemplateDecl()) {
            auto* target = TD->getTemplatedDecl() ? TD->getTemplatedDecl()
                                                  : static_cast<clang::NamedDecl*>(TD);
            occur(out, target, RelationKind::Reference, DTSTL.getTemplateNameLoc());
        }
        return;
    }

    if(auto TSTL = TL.getAs<clang::TemplateSpecializationTypeLoc>()) {
        auto* TST = TSTL.getTypePtr();
        if(TST->isDependentType()) {
            /// The arguments are dependent but the template itself is known;
            /// reference its pattern.
            if(auto* TD = TST->getTemplateName().getAsTemplateDecl()) {
                auto* target = TD->getTemplatedDecl() ? TD->getTemplatedDecl()
                                                      : static_cast<clang::NamedDecl*>(TD);
                occur(out, target, RelationKind::Reference, TSTL.getTemplateNameLoc());
            }
            return;
        }

        /// Prefer the (implicit) specialization over its pattern: hover
        /// renders the concrete type, and the index normalizes back to the
        /// pattern anyway. Alias templates keep the alias itself.
        const clang::NamedDecl* decl = nullptr;
        if(llvm::isa_and_nonnull<clang::ClassTemplateDecl>(
               TST->getTemplateName().getAsTemplateDecl())) {
            decl = TST->getAsCXXRecordDecl();
        }
        if(!decl) {
            decl = ast::decl_of(TSTL.getType());
        }

        occur(out, decl, RelationKind::Reference, TSTL.getTemplateNameLoc());
        return;
    }

    /// std::vector<T>::value_type
    ///                      ^~~~ weak reference, resolved on the primary template
    if(auto DNTL = TL.getAs<clang::DependentNameTypeLoc>()) {
        if(resolver) {
            for(auto* target: resolver->lookup(DNTL.getTypePtr())) {
                occur(out, target, RelationKind::WeakReference, DNTL.getNameLoc());
            }
        }
        return;
    }

    /// std::allocator<T>::rebind<U>
    ///                       ^~~~ weak reference
    if(auto DTSTL = TL.getAs<clang::DependentTemplateSpecializationTypeLoc>()) {
        if(resolver) {
            for(auto* target: resolver->lookup(DTSTL.getTypePtr())) {
                occur(out, target, RelationKind::WeakReference, DTSTL.getTemplateNameLoc());
            }
        }
        return;
    }
}

void nns_occurrences(clang::NestedNameSpecifierLoc NNSL,
                     Occurrences& out,
                     TemplateResolver* resolver) {
    auto* NNS = NNSL.getNestedNameSpecifier();
    switch(NNS->getKind()) {
        case clang::NestedNameSpecifier::Namespace: {
            occur(out, NNS->getAsNamespace(), RelationKind::Reference, NNSL.getLocalBeginLoc());
            break;
        }

        case clang::NestedNameSpecifier::NamespaceAlias: {
            occur(out,
                  NNS->getAsNamespaceAlias(),
                  RelationKind::Reference,
                  NNSL.getLocalBeginLoc());
            break;
        }

        case clang::NestedNameSpecifier::Identifier: {
            assert(NNS->isDependent() && "Identifier NNS should be dependent");
            if(resolver) {
                for(auto* target:
                    resolver->lookup(NNS->getPrefix(),
                                     clang::DeclarationName(NNS->getAsIdentifier()))) {
                    occur(out, target, RelationKind::WeakReference, NNSL.getLocalBeginLoc());
                }
            }
            break;
        }

        case clang::NestedNameSpecifier::Super: {
            /// __super::member (MS extension) — the qualifier names the base
            /// record.
            occur(out, NNS->getAsRecordDecl(), RelationKind::Reference, NNSL.getLocalBeginLoc());
            break;
        }

        case clang::NestedNameSpecifier::TypeSpec:
        case clang::NestedNameSpecifier::Global: {
            break;
        };
    }
}

/// `::new` / `::delete` begin at the `::` token; anchor the occurrence on
/// the keyword itself so hover's exact-location match finds it.
clang::SourceLocation keyword_after_scope(const clang::Decl* decl,
                                          clang::SourceLocation begin,
                                          bool global_qualified) {
    if(!global_qualified || begin.isInvalid() || begin.isMacroID()) {
        return begin;
    }
    auto& context = decl->getASTContext();
    auto token =
        clang::Lexer::findNextToken(begin, context.getSourceManager(), context.getLangOpts());
    return token ? token->getLocation() : begin;
}

void stmt_occurrences(const clang::Stmt* S, Occurrences& out, TemplateResolver* resolver) {
    /// foo = 1
    ///  ^~~~ reference
    if(auto* DRE = llvm::dyn_cast<clang::DeclRefExpr>(S)) {
        occur(out, DRE->getDecl(), RelationKind::Reference, DRE->getLocation());
        return;
    }

    /// sizeof...(Ts)
    ///           ^~~~ reference to the pack declaration
    if(auto* SOPE = llvm::dyn_cast<clang::SizeOfPackExpr>(S)) {
        occur(out, SOPE->getPack(), RelationKind::Reference, SOPE->getPackLoc());
        return;
    }

    /// new Foo / delete p with class-provided allocation functions
    /// ^~~~ reference to operator new / operator delete
    if(auto* NE = llvm::dyn_cast<clang::CXXNewExpr>(S)) {
        if(auto* op = NE->getOperatorNew()) {
            occur(out,
                  op,
                  RelationKind::Reference,
                  keyword_after_scope(op, NE->getBeginLoc(), NE->isGlobalNew()));
        }
        return;
    }
    if(auto* DE = llvm::dyn_cast<clang::CXXDeleteExpr>(S)) {
        if(auto* op = DE->getOperatorDelete()) {
            occur(out,
                  op,
                  RelationKind::Reference,
                  keyword_after_scope(op, DE->getBeginLoc(), DE->isGlobalDelete()));
        }
        return;
    }

    /// Foo value(1); / Foo value{};
    ///          ^~~~ reference to the selected constructor
    if(auto* CE = llvm::dyn_cast<clang::CXXConstructExpr>(S)) {
        /// Implicit constructions have no written parens; the invalid
        /// location drops the occurrence.
        occur(out,
              CE->getConstructor(),
              RelationKind::Reference,
              CE->getParenOrBraceRange().getBegin());
        return;
    }

    /// goto done; / done: ... / &&done
    ///      ^~~~ reference / definition / reference
    if(auto* GS = llvm::dyn_cast<clang::GotoStmt>(S)) {
        occur(out, GS->getLabel(), RelationKind::Reference, GS->getLabelLoc());
        return;
    }
    if(auto* LS = llvm::dyn_cast<clang::LabelStmt>(S)) {
        occur(out, LS->getDecl(), RelationKind::Definition, LS->getIdentLoc());
        return;
    }
    if(auto* ALE = llvm::dyn_cast<clang::AddrLabelExpr>(S)) {
        occur(out, ALE->getLabel(), RelationKind::Reference, ALE->getLabelLoc());
        return;
    }

    /// a == b rewritten from operator<=> / defaulted operator==
    ///   ^~~~ reference to the rewritten-to operator
    if(auto* RBO = llvm::dyn_cast<clang::CXXRewrittenBinaryOperator>(S)) {
        auto decomposed = RBO->getDecomposedForm();
        if(auto* call =
               llvm::dyn_cast_if_present<clang::CXXOperatorCallExpr>(decomposed.InnerBinOp)) {
            if(auto* callee = call->getDirectCallee()) {
                occur(out, callee, RelationKind::Reference, RBO->getOperatorLoc());
            }
        }
        return;
    }

    /// Foo foo{.bar = 1};
    ///          ^~~~ reference
    /// Every designator is emitted with its own location; consumers match
    /// by position, so nested designators need no outer-wins heuristic.
    if(auto* DIE = llvm::dyn_cast<clang::DesignatedInitExpr>(S)) {
        for(const auto& designator: DIE->designators()) {
            if(designator.isFieldDesignator()) {
                occur(out,
                      designator.getFieldDecl(),
                      RelationKind::Reference,
                      designator.getFieldLoc());
            }
        }
        return;
    }

    /// foo.bar
    ///      ^~~~ reference
    if(auto* ME = llvm::dyn_cast<clang::MemberExpr>(S)) {
        auto location = ME->getMemberLoc();
        if(location.isInvalid()) {
            /// An invalid member location means an implicit member expr, e.g.
            /// the implicit `operator bool` call in `if(x)`.
            return;
        }

        occur(out, ME->getMemberDecl(), RelationKind::Reference, location);
        return;
    }

    /// std::is_same<T, U>::value
    ///                       ^~~~ weak reference
    if(auto* DSDRE = llvm::dyn_cast<clang::DependentScopeDeclRefExpr>(S)) {
        if(resolver) {
            for(auto* target: resolver->lookup(DSDRE)) {
                occur(out, target, RelationKind::WeakReference, DSDRE->getNameInfo().getLoc());
            }
        }
        return;
    }

    /// foo(...) / obj.foo(...) where foo names an overload set — clang
    /// already stores the candidates.
    if(auto* OE = llvm::dyn_cast<clang::OverloadExpr>(S)) {
        /// Unwrap using shadows to the underlying functions, then reference
        /// each introducing using-declaration once: hover picks it when the
        /// overload set is otherwise ambiguous.
        llvm::SmallPtrSet<const clang::UsingDecl*, 2> introducers;
        for(auto* target: OE->decls()) {
            if(auto* shadow = llvm::dyn_cast<clang::UsingShadowDecl>(target)) {
                if(auto* UD = llvm::dyn_cast<clang::UsingDecl>(shadow->getIntroducer())) {
                    introducers.insert(UD);
                }
                target = shadow->getTargetDecl();
            }
            occur(out, target, RelationKind::WeakReference, OE->getNameLoc());
        }
        for(const auto* UD: introducers) {
            occur(out, UD, RelationKind::WeakReference, OE->getNameLoc());
        }
        return;
    }

    /// obj(args) / obj[i] with overloaded operators — the traversal skips
    /// these calls' implicit callee references, so the punctuation token has
    /// no other occurrence source.
    if(auto* OCE = llvm::dyn_cast<clang::CXXOperatorCallExpr>(S)) {
        auto op = OCE->getOperator();
        if(op == clang::OO_Call || op == clang::OO_Subscript) {
            /// The callee reference sits on the opening token and
            /// getOperatorLoc on the closing one; anchor both.
            occur(out,
                  OCE->getDirectCallee(),
                  RelationKind::Reference,
                  OCE->getCallee()->getExprLoc());
            occur(out, OCE->getDirectCallee(), RelationKind::Reference, OCE->getOperatorLoc());
        }
        return;
    }

    /// t.foo / this->foo where the base type is dependent
    ///   ^~~~ weak reference, resolved through the template resolver
    if(auto* DSME = llvm::dyn_cast<clang::CXXDependentScopeMemberExpr>(S)) {
        if(resolver) {
            for(auto* target: resolver->lookup(DSME)) {
                occur(out, target, RelationKind::WeakReference, DSME->getMemberLoc());
            }
        }
        return;
    }
}

}  // namespace

llvm::SmallVector<NameOccurrence, 2> resolve_occurrences(const SemanticNode& node,
                                                         TemplateResolver* resolver) {
    llvm::SmallVector<NameOccurrence, 2> out;

    switch(node.kind()) {
        case SemanticNode::Kind::Decl: {
            decl_occurrences(node.get<clang::Decl>(), out, resolver);
            break;
        }

        case SemanticNode::Kind::Stmt: {
            stmt_occurrences(node.get<clang::Stmt>(), out, resolver);
            break;
        }

        case SemanticNode::Kind::TypeLoc: {
            type_loc_occurrences(*node.get<clang::TypeLoc>(), out, resolver);
            break;
        }

        case SemanticNode::Kind::NestedNameSpecifierLoc: {
            nns_occurrences(*node.get<clang::NestedNameSpecifierLoc>(), out, resolver);
            break;
        }

        case SemanticNode::Kind::CtorInitializer: {
            /// Foo() : bar(1) {}
            ///          ^~~~ reference
            auto* init = node.get<clang::CXXCtorInitializer>();
            if(init->isAnyMemberInitializer()) {
                occur(out,
                      init->getAnyMember(),
                      RelationKind::Reference,
                      init->getMemberLocation());
            }
            break;
        }

        case SemanticNode::Kind::TemplateArgumentLoc: {
            /// Foo<Bar> where the argument is itself a template name
            ///      ^~~~ reference
            auto& TAL = *node.get<clang::TemplateArgumentLoc>();
            auto kind = TAL.getArgument().getKind();
            if(kind == clang::TemplateArgument::Template ||
               kind == clang::TemplateArgument::TemplateExpansion) {
                auto template_name = TAL.getArgument().getAsTemplateOrTemplatePattern();
                if(auto* TD = template_name.getAsTemplateDecl()) {
                    occur(out, TD, RelationKind::Reference, TAL.getTemplateNameLoc());
                }
            }
            break;
        }

        case SemanticNode::Kind::ConceptReference: {
            /// requires Foo<T>;
            ///            ^~~~ reference
            auto* reference = node.get<clang::ConceptReference>();
            occur(out,
                  reference->getNamedConcept(),
                  RelationKind::Reference,
                  reference->getConceptNameLoc());
            break;
        }

        default: {
            break;
        }
    }

    return out;
}

}  // namespace clice

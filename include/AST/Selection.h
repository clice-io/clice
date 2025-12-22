#pragma once

#include <stack>

#include "SourceCode.h"

#include "llvm/ADT/SmallVector.h"
#include "clang/AST/ASTTypeTraits.h"
#include "clang/AST/PrettyPrinter.h"
#include "clang/Tooling/Syntax/Tokens.h"

namespace clice {

class CompilationUnit;

/// A selection can partially or completely cover several AST nodes.
/// The SelectionTree contains nodes that are covered, and their parents.
/// SelectionTree does not contain all AST nodes, rather only:
///   Decl, Stmt, TypeLoc, NestedNamespaceSpecifierLoc, CXXCtorInitializer.
/// (These are the nodes with source ranges that fit in DynTypedNode).
///
/// Usually commonAncestor() is the place to start:
///  - it's the simplest answer to "what node is under the cursor"
///  - the selected Expr (for example) can be found by walking up the parent
///    chain and checking Node->ASTNode.
///  - if you want to traverse the selected nodes, they are all under
///    commonAncestor() in the tree.
///
/// SelectionTree tries to behave sensibly in the presence of macros, but does
/// not model any preprocessor concepts: the output is a subset of the AST.
/// When a macro argument is specifically selected, only its first expansion is
/// selected in the AST. (Returning a selection forest is unreasonably difficult
/// for callers to handle correctly.)
///
/// Comments, directives and whitespace are completely ignored.
/// Semicolons are also ignored, as the AST generally does not model them well.
///
/// The SelectionTree owns the Node structures, but the ASTNode attributes
/// point back into the AST it was constructed with.
class SelectionTree {
public:
    /// Create selection trees for the given range, and pass them to Func.
    ///
    /// There may be multiple possible selection trees:
    /// - if the range is empty and borders two tokens, a tree for the right token
    ///   and a tree for the left token will be yielded.
    /// - Func should return true on success (stop) and false on failure (continue)
    ///
    /// Always yields at least one tree. If no tokens are touched, it is empty.
    static bool create_each(CompilationUnit& unit,
                            LocalSourceRange range,
                            llvm::function_ref<bool(SelectionTree)> callback);

    /// Create a selection tree for the given range.
    ///
    /// Where ambiguous (range is empty and borders two tokens), prefer the token
    /// on the right.
    static SelectionTree create_right(CompilationUnit& unit, LocalSourceRange range);

    /// Copies are no good - contain pointers to other nodes.
    SelectionTree(const SelectionTree&) = delete;
    SelectionTree& operator=(const SelectionTree&) = delete;

    /// Moves are OK though - internal storage is pointer-stable when moved.
    SelectionTree(SelectionTree&&) = default;
    SelectionTree& operator=(SelectionTree&&) = default;

    // Describes to what extent an AST node is covered by the selection.
    enum SelectionKind : unsigned char {
        // The AST node owns no characters covered by the selection.
        // Note that characters owned by children don't count:
        //   if (x == 0) scream();
        //       ^^^^^^
        // The IfStmt would be Unselected because all the selected characters are
        // associated with its children.
        // (Invisible nodes like ImplicitCastExpr are always unselected).
        Unselected,
        // The AST node owns selected characters, but is not completely covered.
        Partial,
        // The AST node owns characters, and is covered by the selection.
        Complete,
    };

    // An AST node that is implicated in the selection.
    // (Either selected directly, or some descendant is selected).
    struct Node {
        /// The parent within the selection tree. nullptr for TranslationUnitDecl.
        Node* parent;

        /// Direct children within the selection tree.
        llvm::SmallVector<const Node*> children;

        /// The extent to which this node is covered by the selection.
        SelectionKind selected;

        clang::DynTypedNode data;

        template <typename T>
        auto get() const {
            return data.get<T>();
        }

        /// Get the source range of this node.
        clang::SourceRange source_range() const;

        /// Printable node kind, like "CXXRecordDecl" or "AutoTypeLoc".
        std::string kind() const;

        /// Walk up the AST to get the lexical DeclContext of this Node, which is not
        /// the node itself.
        const clang::DeclContext& decl_context() const;

        /// If this node is a wrapper with no syntax (e.g. implicit cast), return
        /// its contents. (If multiple wrappers are present, unwraps all of them).
        const Node& ignore_implicit() const;

        // If this node is inside a wrapper with no syntax (e.g. implicit cast),
        // return that wrapper. (If multiple are present, unwraps all of them).
        const Node& outer_implicit() const;
    };

    // The most specific common ancestor of all the selected nodes.
    // Returns nullptr if the common ancestor is the root.
    // (This is to avoid accidentally traversing the TUDecl and thus preamble).
    const Node* common_ancestor() const;

    // The selection node corresponding to TranslationUnitDecl.
    const Node& root() const {
        return *m_root;
    }

    void print(llvm::raw_ostream& os, const Node& node, int indent) const;

    friend llvm::raw_ostream& operator<<(llvm::raw_ostream& os, const SelectionTree& tree) {
        tree.print(os, tree.root(), 1);
        return os;
    }

private:
    // Creates a selection tree for the given range in the main file.
    // The range includes bytes [Start, End).
    SelectionTree(CompilationUnit& unit, LocalSourceRange range);

    // Stable-pointer storage, FIXME: use memory pool instead?
    std::deque<Node> nodes;

    const Node* m_root;

    clang::PrintingPolicy print_policy;
};

}  // namespace clice

#pragma once

#include "clang/AST/ASTTypeTraits.h"
#include "clang/Tooling/Syntax/Tokens.h"

#include <deque>

namespace clice {

// Code Action:
// add implementation in cpp file(important).
// extract implementation to cpp file(important).
// generate virtual function declaration(full qualified?).
// generate c++20 coroutine and awaiter interface.
// expand macro(one step by step).
// invert if.

class ASTInfo;

namespace {
class SelectionBuilder;
}

class SelectionTree {
    friend class SelectionBuilder;

public:
    /// The extent to which an selection is covered by the AST node.
    enum class CoverageKind : unsigned {
        /// For example, if the selection is
        ///
        ///  void f() {
        ///     int x = 1;
        ///         ^^^
        ///  }
        ///
        /// The FunctionDecl `f()` and VarDecl `x` would fully cover the selection.
        Full,

        /// For example, if the selection is
        ///
        ///  if (x == 1) {
        ///  ^^^^^^^^^^^^^
        ///     int y = 2;
        ///  }
        ///
        /// The IfStmt would fully cover the selection while the Expr `x == 1` would partially
        /// cover the selection.
        Partial,
    };

    /// An AST node is involved in the selection, either selected directly or some descendant node
    /// is selected.
    struct Node {
        /// The AST node that is selected.
        clang::DynTypedNode dynNode;

        /// The extent to which the selection is covered by the AST node.
        CoverageKind kind;

        /// In most cases, there is only 1 child in a selected node. Use SmallVector with stack
        /// capability 1 to reduce the size of Node.
        llvm::SmallVector<const Node*, 1> children;

        /// The parent node in the selection tree. nullptr for root node.
        Node* parent;

        template <typename T, typename... Ts>
        bool isOneOf() const {
            return dynNode.get<T>() || (dynNode.get<Ts>() || ...);
        }
    };

    /// Construct an empty selection tree.
    SelectionTree() = default;

    SelectionTree(const SelectionTree&) = delete;
    SelectionTree& operator= (const SelectionTree&) = delete;

    SelectionTree(SelectionTree&&) = default;
    SelectionTree& operator= (SelectionTree&&) = default;

    /// Check if there is any selection.
    bool hasValue() const {
        return root != nullptr;
    }

    // Return nullptr if there is no selection.
    const Node* getRoot() const {
        return root;
    }

    std::deque<Node>& children() {
        return storage;
    }

    const std::deque<Node>& children() const {
        return storage;
    }

    /// Return true to continue the walk, false to stop.
    using Walker = llvm::function_ref<bool(const Node*)>;

    /// Return true if the walk is completed, false if the walk is interrupted.
    bool walkDfs(Walker ops) const {
        if(!root)
            return true;

        llvm::SmallVector<const Node*> stack;
        stack.push_back(root);
        while(!stack.empty()) {
            auto node = stack.pop_back_val();

            if(!ops(node))
                return false;

            for(auto child: node->children) {
                stack.push_back(child);
            }
        }

        return true;
    }

    /// Return true if the walk is completed, false if the walk is interrupted.
    bool walkBfs(Walker ops) const {
        if(!root)
            return true;

        std::deque<const Node*> queue;
        queue.push_back(root);

        while(!queue.empty()) {
            auto node = queue.front();
            queue.pop_front();

            if(!ops(node))
                return false;

            for(auto child: node->children) {
                queue.push_front(child);
            }
        }

        return true;
    }

    explicit operator bool () const {
        return hasValue();
    }

    void dump(llvm::raw_ostream& os, clang::ASTContext& context) const;

    static SelectionTree selectOffsetRange(std::uint32_t begin,
                                           std::uint32_t end,
                                           clang::ASTContext& context,
                                           clang::syntax::TokenBuffer& tokens) {
        return SelectionTree(begin, end, context, tokens);
    }

    static SelectionTree selectToken(const clang::syntax::Token& token,
                                     clang::ASTContext& context,
                                     clang::syntax::TokenBuffer& tokens);

private:
    /// Construct a selection tree from the given source range. `start` and `end` means offset from
    /// file start location, these arguments should come from function `SourceConverter::toOffset`.
    SelectionTree(std::uint32_t begin,
                  std::uint32_t end,
                  clang::ASTContext& context,
                  clang::syntax::TokenBuffer& tokens);

    // The root node of selection tree.
    Node* root;

    // The AST nodes was stored in the order from root to leaf.
    // Use deque as the stable pointer storage.
    std::deque<Node> storage;
};

}  // namespace clice

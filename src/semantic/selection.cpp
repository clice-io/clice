#include "semantic/selection.h"

#include <algorithm>
#include <optional>
#include <ranges>
#include <string>
#include <utility>

#include "compile/compilation_unit.h"
#include "semantic/semantics.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/raw_ostream.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/Expr.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/PrettyPrinter.h"
#include "clang/AST/TypeLoc.h"

namespace clice {

namespace {

using Node = SelectionTree::Node;

// Return the range covering a node and all its children.
clang::SourceRange get_source_range(const clang::DynTypedNode& node) {
    // MemberExprs to implicitly access anonymous fields should not claim any
    // tokens for themselves, see claimed_source_range in semantics.cpp.
    if(const auto* ME = node.get<clang::MemberExpr>()) {
        if(!ME->getMemberDecl()->getDeclName()) {
            return ME->getBase() ? get_source_range(clang::DynTypedNode::create(*ME->getBase()))
                                 : clang::SourceRange();
        }
    }
    return node.getSourceRange();
}

// Sentinel value for the selectedness of a node where we've seen no tokens yet.
// This resolves to Unselected if no tokens are ever seen.
// But Unselected + Complete -> Partial, while no_tokens + Complete --> Complete.
// This value is never exposed publicly.
constexpr SelectionTree::SelectionKind no_tokens = static_cast<SelectionTree::SelectionKind>(
    static_cast<unsigned char>(SelectionTree::Complete + 1));

// Nodes start with no_tokens, and then use this function to aggregate the
// selectedness as more tokens are found.
void update(SelectionTree::SelectionKind& result, SelectionTree::SelectionKind new_kind) {
    if(new_kind == no_tokens) {
        return;
    }

    if(result == no_tokens) {
        result = new_kind;
    } else if(result != new_kind) {
        // Can only be completely selected (or unselected) if all tokens are.
        result = SelectionTree::Partial;
    }
}

// Show the type of a node for debugging.
void print_node_kind(llvm::raw_ostream& os, const clang::DynTypedNode& node) {
    if(const clang::TypeLoc* TL = node.get<clang::TypeLoc>()) {
        // TypeLoc is a hierarchy, but has only a single ASTNodeKind.
        // Synthesize the name from the Type subclass (except for QualifiedTypeLoc).
        if(TL->getTypeLocClass() == clang::TypeLoc::Qualified) {
            os << "QualifiedTypeLoc";
        } else {
            os << TL->getType()->getTypeClassName() << "TypeLoc";
        }
    } else {
        os << node.getNodeKind().asStringRef();
    }
}

llvm::SmallString<256> abbreviated_string(clang::DynTypedNode node,
                                          const clang::PrintingPolicy& printing_policy) {
    llvm::SmallString<256> result;
    {
        llvm::raw_svector_ostream os(result);
        node.print(os, printing_policy);
    }

    auto pos = result.find('\n');
    if(pos != llvm::StringRef::npos) {
        bool more_text = !llvm::all_of(result.str().drop_front(pos), llvm::isSpace);
        result.resize(pos);
        if(more_text) {
            result.append(" …");
        }
    }
    return result;
}

}  // namespace

void SelectionTree::print(llvm::raw_ostream& os,
                          const SelectionTree::Node& node,
                          int indent) const {
    if(node.selected) {
        os.indent(indent - 1) << (node.selected == SelectionTree::Complete ? '*' : '.');
    } else {
        os.indent(indent);
    }

    print_node_kind(os, node.data);
    os << ' ' << abbreviated_string(node.data, print_policy) << "\n";
    for(const Node* child: node.children) {
        print(os, *child, indent + 2);
    }
}

std::string SelectionTree::Node::kind() const {
    std::string S;
    llvm::raw_string_ostream OS(S);
    print_node_kind(OS, data);
    return std::move(OS.str());
}

bool SelectionTree::create_each(CompilationUnitRef unit,
                                LocalSourceRange range,
                                llvm::function_ref<bool(SelectionTree)> callback) {
    auto [begin, end] = range;

    if(begin != end) {
        return callback(SelectionTree(unit, range));
    }

    // Decide which selections emulate a "point" query in between characters.
    // If it's ambiguous (the neighboring characters are selectable tokens), returns
    // both possibilities in preference order. Always returns at least one range
    // - if no tokens touched, and empty range.
    llvm::SmallVector<LocalSourceRange, 2> ranges;

    auto location = unit.create_location(unit.main_file(), begin);

    // Prefer right token over left.
    for(const clang::syntax::Token& token: llvm::reverse(unit.spelled_tokens_touch(location))) {
        if(should_ignore_token(token)) {
            continue;
        }

        auto offset = unit.file_offset(token.location());
        ranges.emplace_back(offset, offset + token.length());
    }

    /// Make sure, we have at least one range.
    if(ranges.empty()) {
        ranges.emplace_back(begin, begin);
    }

    for(auto range: ranges) {
        if(callback(SelectionTree(unit, range))) {
            return true;
        }
    }

    return false;
}

SelectionTree SelectionTree::create_right(CompilationUnitRef unit, LocalSourceRange range) {
    std::optional<SelectionTree> result;
    create_each(unit, range, [&](SelectionTree T) {
        result = std::move(T);
        return true;
    });
    return std::move(*result);
}

SelectionTree::SelectionTree(CompilationUnitRef unit, LocalSourceRange range) :
    print_policy(unit.context().getLangOpts()) {
    print_policy.TerseOutput = true;
    print_policy.IncludeNewlines = false;

    const Semantics& semantics = unit.semantics();
    auto tokens = semantics.spelled_tokens();
    auto [begin, end] = range;

    // The root (TranslationUnitDecl) is always present.
    nodes.emplace_back();
    nodes.back().data = clang::DynTypedNode::create(*unit.context().getTranslationUnitDecl());
    nodes.back().parent = nullptr;
    nodes.back().selected = SelectionTree::Unselected;
    root_node = &nodes.front();

    // Find the spelled tokens overlapping the selection, and fold their
    // per-token selectedness into the nodes owning them.
    auto count = static_cast<std::uint32_t>(tokens.size());
    auto indices = std::views::iota(0u, count);
    auto lower = std::ranges::partition_point(indices, [&](std::uint32_t i) {
        return semantics.token_offset(i) + tokens[i].length() <= begin;
    });
    auto first = static_cast<std::uint32_t>(lower - indices.begin());
    // A token is ignored by the algorithm if it can never contribute to any
    // selection: comments, semicolons, cvr-qualifiers, tokens preprocessed
    // to nothing.
    auto ignored = [&](std::uint32_t i) {
        return should_ignore_token(tokens[i]) || semantics.token_preprocessed_away(i);
    };

    llvm::DenseMap<std::uint32_t, std::pair<SelectionKind, std::uint32_t>> hits;
    for(std::uint32_t i = first; i < count && semantics.token_offset(i) < end; i++) {
        if(ignored(i)) {
            continue;
        }

        auto offset = semantics.token_offset(i);
        SelectionKind token_kind = (offset >= begin && offset + tokens[i].length() <= end)
                                       ? SelectionTree::Complete
                                       : SelectionTree::Partial;

        for(std::uint32_t node: semantics.owners(i)) {
            // Preprocessor nodes do not enter the selection tree (yet): the
            // tree stays a subset of the AST.
            if(!semantics.node(node).node.is_ast()) {
                continue;
            }

            auto [it, inserted] = hits.try_emplace(node, std::make_pair(no_tokens, 0u));
            update(it->second.first, token_kind);
            it->second.second++;
        }
    }

    // A node some of whose owned tokens fall outside the selection is only
    // partially covered.
    llvm::SmallVector<std::uint32_t> selected;
    for(auto& [node, state]: hits) {
        if(state.second < semantics.node(node).owned) {
            update(state.first, SelectionTree::Unselected);
        }
        if(state.first == SelectionTree::Partial || state.first == SelectionTree::Complete) {
            selected.push_back(node);
        }
    }

    // Materialize the selected nodes and their ancestor chains. Ancestors
    // always have smaller indices (pre-order), so processing hits in ascending
    // order keeps siblings in traversal order and materializes parents first.
    std::ranges::sort(selected);
    llvm::DenseMap<std::uint32_t, Node*> materialized;

    /// Iterative on purpose: a selected leaf under a deep expression chain
    /// (tens of thousands of nodes) would otherwise recurse once per level
    /// and overflow the stack.
    auto materialize = [&](std::uint32_t index) -> Node* {
        llvm::SmallVector<std::uint32_t> chain;
        auto i = index;
        while(i != Semantics::invalid && !materialized.contains(i)) {
            chain.push_back(i);
            i = semantics.node(i).parent;
        }

        Node* parent = i == Semantics::invalid ? &nodes.front() : materialized[i];
        for(auto it = chain.rbegin(); it != chain.rend(); ++it) {
            nodes.emplace_back();
            Node& node = nodes.back();
            node.data = semantics.node(*it).node.dyn_typed();
            node.parent = parent;
            node.selected = SelectionTree::Unselected;
            parent->children.push_back(&node);
            materialized[*it] = &node;
            parent = &node;
        }
        return parent;
    };

    for(std::uint32_t i: selected) {
        materialize(i)->selected = hits.find(i)->second.first;
    }
}

const Node* SelectionTree::common_ancestor() const {
    const Node* ancestor = root_node;
    while(ancestor->children.size() == 1 && !ancestor->selected) {
        ancestor = ancestor->children.front();
    }

    // Returning nullptr here is a bit unprincipled, but it makes the API safer:
    // the TranslationUnitDecl contains all of the preamble, so traversing it is a
    // performance cliff. Callers can check for null and use root() if they want.
    return ancestor != root_node ? ancestor : nullptr;
}

const clang::DeclContext& SelectionTree::Node::decl_context() const {
    for(const Node* current_node = this; current_node != nullptr;
        current_node = current_node->parent) {
        if(const clang::Decl* current = current_node->get<clang::Decl>()) {
            if(current_node != this) {
                if(auto* DC = dyn_cast<clang::DeclContext>(current)) {
                    return *DC;
                }
            }

            return *current->getLexicalDeclContext();
        }

        if(const auto* LE = current_node->get<clang::LambdaExpr>()) {
            if(current_node != this) {
                return *LE->getCallOperator();
            }
        }
    }
    // A tree must always be rooted at TranslationUnitDecl.
    std::unreachable();
}

clang::SourceRange SelectionTree::Node::source_range() const {
    return get_source_range(data);
}

const SelectionTree::Node& SelectionTree::Node::ignore_implicit() const {
    if(children.size() == 1 && children.front()->source_range() == source_range()) {
        return children.front()->ignore_implicit();
    }

    return *this;
}

const SelectionTree::Node& SelectionTree::Node::outer_implicit() const {
    if(parent && parent->source_range() == source_range()) {
        return parent->outer_implicit();
    }

    return *this;
}

}  // namespace clice

#include "AST/Selection.h"

#include <algorithm>
#include <optional>
#include <set>
#include <string>

#include "Compiler/CompilationUnit.h"
#include "Support/Logging.h"

#include "llvm/ADT/BitVector.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/raw_ostream.h"
#include "clang/AST/ASTConcept.h"
#include "clang/AST/ASTTypeTraits.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/Expr.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/PrettyPrinter.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/AST/TypeLoc.h"
#include "clang/Basic/OperatorKinds.h"
#include "clang/Basic/TokenKinds.h"
#include "clang/Tooling/Syntax/Tokens.h"

namespace clice {

namespace {

using Node = SelectionTree::Node;

std::vector<const clang::Attr*> get_attributes(const clang::DynTypedNode& node) {
    std::vector<const clang::Attr*> attrs;

    if(const auto* TL = node.get<clang::TypeLoc>()) {
        for(auto atl = TL->getAs<clang::AttributedTypeLoc>(); !atl.isNull();
            atl = atl.getModifiedLoc().getAs<clang::AttributedTypeLoc>()) {
            if(const clang::Attr* A = atl.getAttr()) {
                attrs.push_back(A);
            }
            assert(!atl.getModifiedLoc().isNull());
        }
    }

    if(const auto* S = node.get<clang::AttributedStmt>()) {
        for(; S != nullptr; S = dyn_cast<clang::AttributedStmt>(S->getSubStmt())) {
            for(const clang::Attr* A: S->getAttrs()) {
                if(A) {
                    attrs.push_back(A);
                }
            }
        }
    }

    if(const auto* D = node.get<clang::Decl>()) {
        for(const clang::Attr* A: D->attrs()) {
            if(A) {
                attrs.push_back(A);
            }
        }
    }

    return attrs;
}

// Measure the fraction of selections that were enabled by recovery AST.
void record_metrics(const SelectionTree& tree, const clang::LangOptions& lang_opts) {
    /// if(!trace::enabled())
    ///     return;
    /// const char* language_label = lang_opts.CPlusPlus ? "C++" : lang_opts.ObjC ? "ObjC" : "C";
    /// constexpr static trace::Metric selection_used_recovery("selection_recovery",
    ///                                                      trace::Metric::Distribution,
    ///                                                      "language");
    /// constexpr static trace::Metric recovery_type("selection_recovery_type",
    ///                                             trace::Metric::Distribution,
    ///                                             "language");
    /// const auto* common = selection_tree.commonAncestor();
    /// for(const auto* N = common; N; N = N->Parent) {
    ///     if(const auto* RE = N->ASTNode.get<RecoveryExpr>()) {
    ///         selection_used_recovery.record(1, language_label);  // used recovery ast.
    ///         recovery_type.record(RE->isTypeDependent() ? 0 : 1, language_label);
    ///         return;
    ///     }
    /// }
    /// if(common)
    ///     selection_used_recovery.record(0, language_label);  // unused.
}

// Return the range covering a node and all its children.
clang::SourceRange get_source_range(const clang::DynTypedNode& node) {
    // MemberExprs to implicitly access anonymous fields should not claim any
    // tokens for themselves. Given:
    //   struct A { struct { int b; }; };
    // The clang AST reports the following nodes for an access to b:
    //   A().b;
    //   [----] MemberExpr, base = A().<anonymous>, member = b
    //   [----] MemberExpr: base = A(), member = <anonymous>
    //   [-]    CXXConstructExpr
    // For our purposes, we don't want the second MemberExpr to own any tokens,
    // so we reduce its range to match the CXXConstructExpr.
    // (It's not clear that changing the clang AST would be correct in general).
    if(const auto* ME = node.get<clang::MemberExpr>()) {
        if(!ME->getMemberDecl()->getDeclName()) {
            return ME->getBase() ? get_source_range(clang::DynTypedNode::create(*ME->getBase()))
                                 : clang::SourceRange();
        }
    }
    return node.getSourceRange();
}

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
// The tokens actually removed are associated with that node, and hit-tested
// against the selection to determine whether the node is selected.
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

// As well as comments, don't count semicolons as real tokens.
// They're not properly claimed as expr-statement is missing from the AST.
bool should_ignore(const clang::syntax::Token& token) {
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

// SelectionTester can determine whether a range of tokens from the PP-expanded
// stream (corresponding to an AST node) is considered selected.
//
// When the tokens result from macro expansions, the appropriate tokens in the
// main file are examined (macro invocation or args). Similarly for #includes.
// However, only the first expansion of a given spelled token is considered
// selected.
//
// It tests each token in the range (not just the endpoints) as contiguous
// expanded tokens may not have contiguous spellings (with macros).
//
// Non-token text, and tokens not modeled in the AST (comments, semicolons)
// are ignored when determining selectedness.
class SelectionTester {
public:
    // The selection is offsets [SelBegin, SelEnd) in SelFile.
    SelectionTester(CompilationUnit& unit,
                    clang::FileID selected_file_id,
                    LocalSourceRange selected_range,
                    const clang::SourceManager& source_manager) :
        selected_file(selected_file_id),
        selected_file_range(source_manager.getLocForStartOfFile(selected_file_id),
                            source_manager.getLocForEndOfFile(selected_file_id)),
        SM(source_manager) {
        // Find all tokens (partially) selected in the file.
        auto spelled_tokens = unit.spelled_tokens(selected_file);

        const clang::syntax::Token* first =
            llvm::partition_point(spelled_tokens, [&](const clang::syntax::Token& token) {
                return unit.file_offset(token.endLocation()) <= selected_range.begin;
            });

        const clang::syntax::Token* last =
            std::partition_point(first,
                                 spelled_tokens.end(),
                                 [&](const clang::syntax::Token& token) {
                                     return unit.file_offset(token.location()) < selected_range.end;
                                 });

        auto selected_tokens = llvm::ArrayRef(first, last);

        // Find which of these are preprocessed to nothing and should be ignored.
        llvm::BitVector pp_ignored(selected_tokens.size(), false);

        for(const clang::syntax::TokenBuffer::Expansion& expansion:
            unit.expansions_overlapping(selected_tokens)) {
            if(expansion.Expanded.empty()) {
                for(const clang::syntax::Token& token: expansion.Spelled) {
                    if(&token >= first && &token < last) {
                        pp_ignored[&token - first] = true;
                    }
                }
            }
        }

        // Precompute selectedness and offset for selected spelled tokens.
        for(unsigned I = 0; I < selected_tokens.size(); ++I) {
            if(should_ignore(selected_tokens[I]) || pp_ignored[I]) {
                continue;
            }

            selected_spelled.emplace_back();
            Tok& token = selected_spelled.back();
            token.offset = unit.file_offset(selected_tokens[I].location());

            if(token.offset >= selected_range.begin &&
               token.offset + selected_tokens[I].length() <= selected_range.end) {
                token.selected = SelectionTree::Complete;
            } else {
                token.selected = SelectionTree::Partial;
            }
        }

        maybe_selected_expanded = compute_maybe_selected_expanded_tokens(unit.token_buffer());
    }

    // Test whether a consecutive range of tokens is selected.
    // The tokens are taken from the expanded token stream.
    SelectionTree::SelectionKind test(llvm::ArrayRef<clang::syntax::Token> expanded_tokens) const {
        if(expanded_tokens.empty()) {
            return no_tokens;
        }

        if(selected_spelled.empty()) {
            return SelectionTree::Unselected;
        }

        // Cheap (pointer) check whether any of the tokens could touch selection.
        // In most cases, the node's overall source range touches ExpandedTokens,
        // or we would have failed mayHit(). However now we're only considering
        // the *unclaimed* spans of expanded tokens.
        // This is a significant performance improvement when a lot of nodes
        // surround the selection, including when generated by macros.
        if(maybe_selected_expanded.empty() ||
           &expanded_tokens.front() > &maybe_selected_expanded.back() ||
           &expanded_tokens.back() < &maybe_selected_expanded.front()) {
            return SelectionTree::Unselected;
        }

        // The eof token is used as a sentinel.
        // In general, source range from an AST node should not claim the eof token,
        // but it could occur for unmatched-bracket cases.
        // FIXME: fix it in TokenBuffer, expandedTokens(SourceRange) should not
        // return the eof token.
        if(expanded_tokens.back().kind() == clang::tok::eof) {
            expanded_tokens = expanded_tokens.drop_back();
        }

        SelectionTree::SelectionKind result = no_tokens;

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

            update(result, test_chunk(fid, batch));
        }

        return result;
    }

    // Cheap check whether any of the tokens in R might be selected.
    // If it returns false, test() will return no_tokens or Unselected.
    // If it returns true, test() may return any value.
    bool may_hit(clang::SourceRange range) const {
        if(selected_spelled.empty() || maybe_selected_expanded.empty()) {
            return false;
        }

        // If the node starts after the selection ends, it is not selected.
        // Tokens a macro location might claim are >= its expansion start.
        // So if the expansion start > last selected token, we can prune it.
        // (This is particularly helpful for GTest's TEST macro).
        if(auto B = offset_in_sel_file(get_expansion_start(range.getBegin()))) {
            if(*B > selected_spelled.back().offset) {
                return false;
            }
        }

        // If the node ends before the selection begins, it is not selected.
        clang::SourceLocation end_loc = range.getEnd();
        while(end_loc.isMacroID()) {
            end_loc = SM.getImmediateExpansionRange(end_loc).getEnd();
        }

        // In the rare case that the expansion range is a char range, end_loc is
        // ~one token too far to the right. We may fail to prune, that's OK.
        if(auto E = offset_in_sel_file(end_loc)) {
            if(*E < selected_spelled.front().offset) {
                return false;
            }
        }

        return true;
    }

private:
    // Plausible expanded tokens that might be affected by the selection.
    // This is an overestimate, it may contain tokens that are not selected.
    // The point is to allow cheap pruning in test()
    llvm::ArrayRef<clang::syntax::Token>
        compute_maybe_selected_expanded_tokens(const clang::syntax::TokenBuffer& token_buffer) {
        if(selected_spelled.empty())
            return {};

        auto last_affected_token = [&](clang::SourceLocation location) {
            auto offset = offset_in_sel_file(location);
            while(location.isValid() && !offset) {
                location = location.isMacroID() ? SM.getImmediateExpansionRange(location).getEnd()
                                                : SM.getIncludeLoc(SM.getFileID(location));
                offset = offset_in_sel_file(location);
            }
            return offset;
        };

        auto first_affected_token = [&](clang::SourceLocation location) {
            auto offset = offset_in_sel_file(location);
            while(location.isValid() && !offset) {
                location = location.isMacroID() ? SM.getImmediateExpansionRange(location).getBegin()
                                                : SM.getIncludeLoc(SM.getFileID(location));
                offset = offset_in_sel_file(location);
            }
            return offset;
        };

        const clang::syntax::Token* start = llvm::partition_point(
            token_buffer.expandedTokens(),
            [&, first = selected_spelled.front().offset](const clang::syntax::Token& token) {
                if(token.kind() == clang::tok::eof) {
                    return false;
                }

                // Implausible if upperbound(Tok) < First.
                if(auto offset = last_affected_token(token.location())) {
                    return *offset < first;
                }

                // A prefix of the expanded tokens may be from an implicit
                // inclusion (e.g. preamble patch, or command-line -include).
                return true;
            });

        bool end_invalid = false;
        const clang::syntax::Token* end = std::partition_point(
            start,
            token_buffer.expandedTokens().end(),
            [&, last = selected_spelled.back().offset](const clang::syntax::Token& token) {
                if(token.kind() == clang::tok::eof) {
                    return false;
                }

                // Plausible if lowerbound(Tok) <= Last.
                if(auto offset = first_affected_token(token.location())) {
                    return *offset <= last;
                }

                // Shouldn't happen: once we've seen tokens traceable to the main
                // file, there shouldn't be any more implicit inclusions.
                assert(false && "Expanded token could not be resolved to main file!");
                end_invalid = true;
                return true;  // conservatively assume this token can overlap
            });
        if(end_invalid) {
            end = token_buffer.expandedTokens().end();
        }

        return llvm::ArrayRef(start, end);
    }

    // Hit-test a consecutive range of tokens from a single file ID.
    SelectionTree::SelectionKind test_chunk(clang::FileID fid,
                                            llvm::ArrayRef<clang::syntax::Token> batch) const {
        assert(!batch.empty());
        clang::SourceLocation start_loc = batch.front().location();
        // There are several possible categories of FileID depending on how the
        // preprocessor was used to generate these tokens:
        //   main file, #included file, macro args, macro bodies.
        // We need to identify the main-file tokens that represent Batch, and
        // determine whether we want to exclusively claim them. Regular tokens
        // represent one AST construct, but a macro invocation can represent many.

        // Handle tokens written directly in the main file.
        if(fid == selected_file) {
            return test_token_range(*offset_in_sel_file(batch.front().location()),
                                    *offset_in_sel_file(batch.back().location()));
        }

        // Handle tokens in another file #included into the main file.
        // Check if the #include is selected, but don't claim it exclusively.
        if(start_loc.isFileID()) {
            for(clang::SourceLocation Loc = batch.front().location(); Loc.isValid();
                Loc = SM.getIncludeLoc(SM.getFileID(Loc))) {
                if(auto offset = offset_in_sel_file(Loc))
                    // FIXME: use whole #include directive, not just the filename string.
                    return test_token(*offset);
            }
            return no_tokens;
        }

        assert(start_loc.isMacroID());
        // Handle tokens that were passed as a macro argument.
        clang::SourceLocation arg_start = SM.getTopMacroCallerLoc(start_loc);
        if(auto arg_offset = offset_in_sel_file(arg_start)) {
            if(is_first_expansion(fid, arg_start, SM)) {
                clang::SourceLocation arg_end = SM.getTopMacroCallerLoc(batch.back().location());
                return test_token_range(*arg_offset, *offset_in_sel_file(arg_end));
            } else {  // NOLINT(llvm-else-after-return)
                      /* fall through and treat as part of the macro body */
            }
        }

        // Handle tokens produced by non-argument macro expansion.
        // Check if the macro name is selected, don't claim it exclusively.
        if(auto expansion_offset = offset_in_sel_file(get_expansion_start(start_loc))) {
            // FIXME: also check ( and ) for function-like macros?
            return test_token(*expansion_offset);
        }

        return no_tokens;
    }

    // Is the closed token range [Begin, End] selected?
    SelectionTree::SelectionKind test_token_range(unsigned begin, unsigned end) const {
        assert(begin <= end);
        // Outside the selection entirely?
        if(end < selected_spelled.front().offset || begin > selected_spelled.back().offset) {
            return SelectionTree::Unselected;
        }

        // Compute range of tokens.
        auto B =
            llvm::partition_point(selected_spelled, [&](const Tok& T) { return T.offset < begin; });
        auto E = std::partition_point(B, selected_spelled.end(), [&](const Tok& T) {
            return T.offset <= end;
        });

        // Aggregate selectedness of tokens in range.
        bool extends_outside_selection =
            begin < selected_spelled.front().offset || end > selected_spelled.back().offset;
        SelectionTree::SelectionKind result =
            extends_outside_selection ? SelectionTree::Unselected : no_tokens;
        for(auto It = B; It != E; ++It) {
            update(result, It->selected);
        }

        return result;
    }

    // Is the token at `offset` selected?
    SelectionTree::SelectionKind test_token(unsigned offset) const {
        // Outside the selection entirely?
        if(offset < selected_spelled.front().offset || offset > selected_spelled.back().offset) {
            return SelectionTree::Unselected;
        }

        // Find the token, if it exists.
        auto It = llvm::partition_point(selected_spelled,
                                        [&](const Tok& T) { return T.offset < offset; });
        if(It != selected_spelled.end() && It->offset == offset) {
            return It->selected;
        }

        return no_tokens;
    }

    // Decomposes Loc and returns the offset if the file ID is SelFile.
    std::optional<unsigned> offset_in_sel_file(clang::SourceLocation location) const {
        // Decoding Loc with SM.getDecomposedLoc is relatively expensive.
        // But SourceLocations for a file are numerically contiguous, so we
        // can use cheap integer operations instead.
        if(location < selected_file_range.getBegin() || location >= selected_file_range.getEnd()) {
            return std::nullopt;
        }

        // FIXME: subtracting getRawEncoding() is dubious, move this logic into SM.
        return location.getRawEncoding() - selected_file_range.getBegin().getRawEncoding();
    }

    clang::SourceLocation get_expansion_start(clang::SourceLocation location) const {
        while(location.isMacroID()) {
            location = SM.getImmediateExpansionRange(location).getBegin();
        }
        return location;
    }

    struct Tok {
        unsigned offset;
        SelectionTree::SelectionKind selected;
    };

    std::vector<Tok> selected_spelled;
    llvm::ArrayRef<clang::syntax::Token> maybe_selected_expanded;
    clang::FileID selected_file;
    clang::SourceRange selected_file_range;
    const clang::SourceManager& SM;
};

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

/// FIXME: Remove in release mode?
std::string print_node_to_string(const clang::DynTypedNode& node,
                                 const clang::PrintingPolicy& printing_policy) {
    std::string S;
    llvm::raw_string_ostream OS(S);
    print_node_kind(OS, node);
    return std::move(OS.str());
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

// We find the selection by visiting written nodes in the AST, looking for nodes
// that intersect with the selected character range.
//
// While traversing, we maintain a parent stack. As nodes pop off the stack,
// we decide whether to keep them or not. To be kept, they must either be
// selected or contain some nodes that are.
//
// For simple cases (not inside macros) we prune subtrees that don't intersect.
class SelectionVisitor : public clang::RecursiveASTVisitor<SelectionVisitor> {
public:
    // Runs the visitor to gather selected nodes and their ancestors.
    // If there is any selection, the root (TUDecl) is the first node.
    static std::deque<Node> collect(CompilationUnit& unit,
                                    const clang::PrintingPolicy& printing_policy,
                                    LocalSourceRange range,
                                    clang::FileID fid) {
        SelectionVisitor V(unit, printing_policy, range, fid);
        V.TraverseAST(unit.context());
        assert(V.stack.size() == 1 && "Unpaired push/pop?");
        assert(V.stack.top() == &V.nodes.front());
        return std::move(V.nodes);
    }

    // We traverse all "well-behaved" nodes the same way:
    //  - push the node onto the stack
    //  - traverse its children recursively
    //  - pop it from the stack
    //  - hit testing: is intersection(node, selection) - union(children) empty?
    //  - attach it to the tree if it or any children hit the selection
    //
    // Two categories of nodes are not "well-behaved":
    //  - those without source range information, we don't record those
    //  - those that can't be stored in DynTypedNode.
    bool TraverseDecl(clang::Decl* X) {
        // Already pushed by constructor.
        if(llvm::isa_and_nonnull<clang::TranslationUnitDecl>(X)) {
            /// Top level decls cover the selection.
            for(auto decl: unit.top_level_decls()) {
                if(!TraverseDecl(decl)) {
                    return false;
                }
            }
            return true;
        }

        // Base::TraverseDecl will suppress children, but not this node itself.
        if(X && X->isImplicit()) {
            // Most implicit nodes have only implicit children and can be skipped.
            // However there are exceptions (`void foo(Concept auto x)`), and
            // the base implementation knows how to find them.
            return Base::TraverseDecl(X);
        }

        return traverse_node(X, [&] { return Base::TraverseDecl(X); });
    }

    bool TraverseTypeLoc(clang::TypeLoc X) {
        return traverse_node(&X, [&] { return Base::TraverseTypeLoc(X); });
    }

    bool TraverseTemplateArgumentLoc(const clang::TemplateArgumentLoc& X) {
        return traverse_node(&X, [&] { return Base::TraverseTemplateArgumentLoc(X); });
    }

    bool TraverseNestedNameSpecifierLoc(clang::NestedNameSpecifierLoc X) {
        return traverse_node(&X, [&] { return Base::TraverseNestedNameSpecifierLoc(X); });
    }

    bool TraverseConstructorInitializer(clang::CXXCtorInitializer* X) {
        return traverse_node(X, [&] { return Base::TraverseConstructorInitializer(X); });
    }

    bool TraverseCXXBaseSpecifier(const clang::CXXBaseSpecifier& X) {
        return traverse_node(&X, [&] { return Base::TraverseCXXBaseSpecifier(X); });
    }

    bool TraverseAttr(clang::Attr* X) {
        return traverse_node(X, [&] { return Base::TraverseAttr(X); });
    }

    bool TraverseConceptReference(clang::ConceptReference* X) {
        return traverse_node(X, [&] { return Base::TraverseConceptReference(X); });
    }

    // Stmt is the same, but this form allows the data recursion optimization.
    bool dataTraverseStmtPre(clang::Stmt* X) {
        if(!X || is_implicit(X)) {
            return false;
        }

        auto N = clang::DynTypedNode::create(*X);
        if(safely_skipable(N)) {
            return false;
        }

        push(std::move(N));
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
        return traverse_node<clang::TypeLoc>(&QX, [&] {
            return TraverseTypeLoc(QX.getUnqualifiedLoc());
        });
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
        return traverse_node(S, [&] {
            return TraverseStmt(S->getInit()) && TraverseDecl(S->getLoopVariable()) &&
                   TraverseStmt(S->getRangeInit()) && TraverseStmt(S->getBody());
        });
    }

    // OpaqueValueExpr blocks traversal, we must explicitly traverse it.
    bool TraverseOpaqueValueExpr(clang::OpaqueValueExpr* E) {
        return traverse_node(E, [&] { return TraverseStmt(E->getSourceExpr()); });
    }

    // We only want to traverse the *syntactic form* to understand the selection.
    bool TraversePseudoObjectExpr(clang::PseudoObjectExpr* E) {
        return traverse_node(E, [&] { return TraverseStmt(E->getSyntacticForm()); });
    }

    bool TraverseTypeConstraint(const clang::TypeConstraint* C) {
        if(auto* E = C->getImmediatelyDeclaredConstraint()) {
            // Technically this expression is 'implicit' and not traversed by the RAV.
            // However, the range is correct, so we visit expression to avoid adding
            // an extra kind to 'DynTypeNode' that hold 'TypeConstraint'.
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
    using Base = RecursiveASTVisitor<SelectionVisitor>;

    SelectionVisitor(CompilationUnit& unit,
                     const clang::PrintingPolicy& printing_policy,
                     LocalSourceRange range,
                     clang::FileID selected_file) :
        unit(unit), SM(unit.context().getSourceManager()), lang_opts(unit.context().getLangOpts()),
        print_policy(printing_policy), checker(unit, selected_file, range, SM),
        unclaimed_expanded_tokens(unit.expanded_tokens()) {
        // Ensure we have a node for the TU decl, regardless of traversal scope.
        nodes.emplace_back();
        nodes.back().data = clang::DynTypedNode::create(*unit.context().getTranslationUnitDecl());
        nodes.back().parent = nullptr;
        nodes.back().selected = SelectionTree::Unselected;
        stack.push(&nodes.back());
    }

    // Generic case of TraverseFoo. Func should be the call to Base::TraverseFoo.
    // Node is always a pointer so the generic code can handle any null checks.
    template <typename T, typename Func>
    bool traverse_node(T* node, const Func& Body) {
        if(node == nullptr) {
            return true;
        }

        auto N = clang::DynTypedNode::create(*node);
        if(safely_skipable(N)) {
            return true;
        }

        push(std::move(N));
        bool ret = Body();
        pop();
        return ret;
    }

    // HIT TESTING
    //
    // We do rough hit testing on the way down the tree to avoid traversing
    // subtrees that don't touch the selection (canSafelySkipNode), but
    // fine-grained hit-testing is mostly done on the way back up (in pop()).
    // This means children get to claim parts of the selection first, and parents
    // are only selected if they own tokens that no child owned.
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

    // An optimization for a common case: nodes outside macro expansions that
    // don't intersect the selection may be recursively skipped.
    bool safely_skipable(const clang::DynTypedNode& N) {
        clang::SourceRange S = get_source_range(N);
        if(auto* TL = N.get<clang::TypeLoc>()) {
            // FIXME: TypeLoc::getBeginLoc()/getEndLoc() are pretty fragile
            // heuristics. We should consider only pruning critical TypeLoc nodes, to
            // be more robust.

            // AttributedTypeLoc may point to the attribute's range, NOT the modified
            // type's range.
            if(auto AT = TL->getAs<clang::AttributedTypeLoc>()) {
                S = AT.getModifiedLoc().getSourceRange();
            }
        }
        // SourceRange often doesn't manage to accurately cover attributes.
        // Fortunately, attributes are rare.
        if(llvm::any_of(get_attributes(N), [](const clang::Attr* A) { return !A->isImplicit(); })) {
            return false;
        }

        if(!checker.may_hit(S)) {
            LOG_DEBUG("{2}skip: {0} {1}",
                      print_node_to_string(N, print_policy),
                      S.printToString(SM),
                      indent());
            return true;
        }

        return false;
    }

    // There are certain nodes we want to treat as leaves in the SelectionTree,
    // although they do have children.
    bool should_skip_children(const clang::Stmt* X) const {
        // UserDefinedLiteral (e.g. 12_i) has two children (12 and _i).
        // Unfortunately TokenBuffer sees 12_i as one token and can't split it.
        // So we treat UserDefinedLiteral as a leaf node, owning the token.
        return llvm::isa<clang::UserDefinedLiteral>(X);
    }

    // Pushes a node onto the ancestor stack. Pairs with pop().
    // Performs early hit detection for some nodes (on the earlySourceRange).
    void push(clang::DynTypedNode node) {
        clang::SourceRange Early = early_source_range(node);
        LOG_DEBUG("{2}push: {0} {1}",
                  print_node_to_string(node, print_policy),
                  node.getSourceRange().printToString(SM),
                  indent());
        nodes.emplace_back();
        nodes.back().data = std::move(node);
        nodes.back().parent = stack.top();
        nodes.back().selected = no_tokens;
        stack.push(&nodes.back());
        claim_range(Early, nodes.back().selected);
    }

    // Pops a node off the ancestor stack, and finalizes it. Pairs with push().
    // Performs primary hit detection.
    void pop() {
        Node& N = *stack.top();
        LOG_DEBUG("{1}pop: {0}", print_node_to_string(N.data, print_policy), indent(-1));
        claim_tokens_for(N.data, N.selected);
        if(N.selected == no_tokens) {
            N.selected = SelectionTree::Unselected;
        }

        if(N.selected || !N.children.empty()) {
            // Attach to the tree.
            N.parent->children.push_back(&N);
        } else {
            // Neither N any children are selected, it doesn't belong in the tree.
            assert(&N == &nodes.back());
            nodes.pop_back();
        }

        stack.pop();
    }

    // Returns the range of tokens that this node will claim directly, and
    // is not available to the node's children.
    // Usually empty, but sometimes children cover tokens but shouldn't own them.
    clang::SourceRange early_source_range(const clang::DynTypedNode& N) {
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
    // By default this claims all unclaimed tokens in getSourceRange().
    // We override this if we want to claim fewer tokens (e.g. there are gaps).
    void claim_tokens_for(const clang::DynTypedNode& N, SelectionTree::SelectionKind& result) {
        // CXXConstructExpr often shows implicit construction, like `string s;`.
        // Don't associate any tokens with it unless there's some syntax like {}.
        // This prevents it from claiming 's', its primary location.
        if(const auto* CCE = N.get<clang::CXXConstructExpr>()) {
            claim_range(CCE->getParenOrBraceRange(), result);
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
                claim_range(PTL.getLParenLoc(), result);
                claim_range(PTL.getRParenLoc(), result);
                return;
            }

            if(auto ATL = TL->getAs<clang::ArrayTypeLoc>()) {
                claim_range(ATL.getBracketsRange(), result);
                return;
            }

            if(auto PTL = TL->getAs<clang::PointerTypeLoc>()) {
                claim_range(PTL.getStarLoc(), result);
                return;
            }

            if(auto FTL = TL->getAs<clang::FunctionTypeLoc>()) {
                claim_range(clang::SourceRange(FTL.getLParenLoc(), FTL.getEndLoc()), result);
                return;
            }
        }

        claim_range(get_source_range(N), result);
    }

    // Perform hit-testing of a complete Node against the selection.
    // This runs for every node in the AST, and must be fast in common cases.
    // This is usually called from pop(), so we can take children into account.
    // The existing state of Result is relevant.
    void claim_range(clang::SourceRange S, SelectionTree::SelectionKind& result) {
        for(const auto& claimed_range: unclaimed_expanded_tokens.erase(unit.expanded_tokens(S))) {
            update(result, checker.test(claimed_range));
        }

        if(result && result != no_tokens) {
            LOG_DEBUG("{1}hit selection: {0}", S.printToString(SM), indent());
        }
    }

    std::string indent(int offset = 0) {
        // Cast for signed arithmetic.
        int amount = int(stack.size()) + offset;
        assert(amount >= 0);
        return std::string(amount, ' ');
    }

    clang::SourceManager& SM;
    const clang::LangOptions& lang_opts;
    const clang::PrintingPolicy& print_policy;
    CompilationUnit& unit;
    std::stack<Node*> stack;
    SelectionTester checker;
    IntervalSet unclaimed_expanded_tokens;
    std::deque<Node> nodes;  // Stable pointers as we add more nodes.
};

}  // namespace

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

bool SelectionTree::create_each(CompilationUnit& unit,
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

    auto location = unit.create_location(unit.interested_file(), begin);

    // Prefer right token over left.
    for(const clang::syntax::Token& token: llvm::reverse(unit.spelled_tokens_touch(location))) {
        if(should_ignore(token)) {
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

SelectionTree SelectionTree::create_right(CompilationUnit& unit, LocalSourceRange range) {
    std::optional<SelectionTree> result;
    create_each(unit, range, [&](SelectionTree T) {
        result = std::move(T);
        return true;
    });
    return std::move(*result);
}

SelectionTree::SelectionTree(CompilationUnit& unit, LocalSourceRange range) :
    print_policy(unit.context().getLangOpts()) {
    // No fundamental reason the selection needs to be in the main file,
    // but that's all clice has needed so far.
    const clang::SourceManager& SM = unit.context().getSourceManager();
    clang::FileID fid = SM.getMainFileID();
    print_policy.TerseOutput = true;
    print_policy.IncludeNewlines = false;
    auto [begin, end] = range;

    LOG_DEBUG("Computing selection for {0}",
              clang::SourceRange(SM.getComposedLoc(fid, begin), SM.getComposedLoc(fid, end))
                  .printToString(SM));

    nodes = SelectionVisitor::collect(unit, print_policy, range, fid);
    m_root = nodes.empty() ? nullptr : &nodes.front();
    record_metrics(*this, unit.context().getLangOpts());
    /// FIXME: dlog("Built selection tree\n{0}", *this);
}

const Node* SelectionTree::common_ancestor() const {
    const Node* ancestor = m_root;
    while(ancestor->children.size() == 1 && !ancestor->selected) {
        ancestor = ancestor->children.front();
    }

    // Returning nullptr here is a bit unprincipled, but it makes the API safer:
    // the TranslationUnitDecl contains all of the preamble, so traversing it is a
    // performance cliff. Callers can check for null and use root() if they want.
    return ancestor != m_root ? ancestor : nullptr;
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
    llvm_unreachable("A tree must always be rooted at TranslationUnitDecl.");
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

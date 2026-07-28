#pragma once

#include "semantic/relation_kind.h"
#include "semantic/semantics.h"

#include "llvm/ADT/STLFunctionalExtras.h"

namespace clang {

class NamedDecl;

}

namespace clice {

/// A name occurrence: which decl the written name refers to, in what role,
/// and where the name token is.
using OccurrenceCallback =
    llvm::function_ref<void(const clang::NamedDecl*, RelationKind, clang::SourceLocation)>;

/// A relation fact: `decl` relates to `target` with `kind` at `range`.
/// Occurrences are mirrored as self-relations (matching the historical index
/// rows); decl-pair facts cover TypeDefinition, Base/Derived,
/// Interface/Implementation, Constructor/Destructor and Caller/Callee.
using RelationCallback = llvm::function_ref<
    void(const clang::NamedDecl*, RelationKind, const clang::NamedDecl*, clang::SourceRange)>;

/// Report every fact `node` gives rise to.
///
/// This is the single implementation of "node → referenced decl" (the
/// distilled content of the former SemanticVisitor visit methods). Consumers
/// derive their own views from it: semantic tokens classify the occurrence
/// decls, the index projection hashes both facts into its rows.
///
/// `enclosing_function` is the nearest enclosing function of the node (walk
/// the semantic map's parent chain); it feeds Caller/Callee facts and may be
/// null. Nodes from implicit instantiations and dependent contexts currently
/// produce no facts, mirroring the previous behavior.
void resolve_facts(const SemanticNode& node,
                   OccurrenceCallback occurrence,
                   RelationCallback relation,
                   const clang::NamedDecl* enclosing_function = nullptr);

/// Report only the name occurrences of `node`.
void resolve_occurrences(const SemanticNode& node, OccurrenceCallback callback);

}  // namespace clice

module;

#include "modules/prelude.h"

/// The call graph and the type hierarchy around a symbol, and the functions
/// enclosing sites.

module clice:index.hierarchy;

import :index.query;
import :index.site;
import :index.symbol;

namespace clice::index {

/// The qualified name of the function whose definition holds each site,
/// the innermost; empty where none does. A function's own name lies in
/// none.
std::vector<std::string> enclosing_functions(const IndexQuery& index, llvm::ArrayRef<Site> sites);

/// One neighbour of a symbol in a graph: the symbol at its canonical
/// site and the sites of the relations that connect them.
struct CallEdge {
    IndexQuery::Located symbol;
    std::vector<Site> sites;
};

/// The functions calling `root` and the ones it calls, with the call
/// sites; only the sides asked for are walked. A method's callers
/// include those of the methods it overrides, whose calls may dispatch
/// to it, but not one naming that method by qualification. A
/// variable, a field or an enumerator has as callers the functions
/// using it, a function's local none. A neighbour no source places is
/// left out.
struct CallGraphOptions {
    bool callers = true;
    bool callees = true;
};

struct CallGraph {
    std::vector<CallEdge> callers;
    std::vector<CallEdge> callees;
};

CallGraph call_graph(const IndexQuery& index, SymbolHash root, Fid anchor, CallGraphOptions options);

/// The types `root` derives from and the ones deriving from it, each
/// at its canonical site; only the sides asked for are walked.
struct TypeHierarchyOptions {
    bool supertypes = true;
    bool subtypes = true;
};

struct TypeHierarchy {
    std::vector<IndexQuery::Located> supertypes;
    std::vector<IndexQuery::Located> subtypes;
};

TypeHierarchy type_hierarchy(const IndexQuery& index, SymbolHash root, Fid anchor, TypeHierarchyOptions options);

}  // namespace clice::index

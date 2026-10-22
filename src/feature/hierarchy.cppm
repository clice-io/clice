module;

#include "modules/prelude.h"

/// The call graph and the type hierarchy around a symbol, and the functions
/// enclosing sites.

module clice:feature.hierarchy;

import :index.query;
import :index.site;
import :index.symbol;

namespace clice::feature {

namespace protocol = kota::ipc::protocol;

/// The qualified name of the function whose definition holds each site,
/// the innermost; empty where none does. A function's own name lies in
/// none.
std::vector<std::string> enclosing_functions(const index::IndexQuery& index,
                                             llvm::ArrayRef<index::Site> sites);

/// One neighbour of a symbol in a graph: the symbol at its canonical
/// site and the sites of the relations that connect them.
struct CallEdge {
    index::IndexQuery::Located symbol;
    std::vector<index::Site> sites;
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

CallGraph call_graph(const index::IndexQuery& index,
                     index::SymbolHash root,
                     Fid anchor,
                     CallGraphOptions options);

/// The types `root` derives from and the ones deriving from it, each
/// at its canonical site; only the sides asked for are walked.
struct TypeHierarchyOptions {
    bool supertypes = true;
    bool subtypes = true;
};

struct TypeHierarchy {
    std::vector<index::IndexQuery::Located> supertypes;
    std::vector<index::IndexQuery::Located> subtypes;
};

TypeHierarchy type_hierarchy(const index::IndexQuery& index,
                             index::SymbolHash root,
                             Fid anchor,
                             TypeHierarchyOptions options);

/// Hierarchy items span the whole declaration (`extent`) and select its
/// name (`site`); the detail shows a symbol's `qualified` name inside a
/// namespace or a class, and a function's signature. They carry their
/// symbol handle in `data` (see data_symbol).
protocol::CallHierarchyItem call_hierarchy_item(const index::SymbolInfo& symbol,
                                                const index::Site& site,
                                                const index::Site& extent,
                                                llvm::StringRef qualified);
protocol::TypeHierarchyItem type_hierarchy_item(const index::SymbolInfo& symbol,
                                                const index::Site& site,
                                                const index::Site& extent,
                                                llvm::StringRef qualified);

}  // namespace clice::feature

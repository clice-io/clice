module;

/// The call graph and the type hierarchy around a symbol, and the functions
/// enclosing sites.

module clice:feature.hierarchy;

import :index.query;
import :index.site;
import :index.symbol;
import :support.prelude;
import :syntax.position;

namespace clice::feature {

/// The qualified name of the function whose definition holds each site,
/// the innermost; empty where none does. A function's own name lies in
/// none.
std::vector<std::string> enclosing_functions(const index::IndexQuery& index,
                                             llvm::ArrayRef<index::Site> sites);

/// One neighbour of a symbol in a graph: the symbol at one of its places
/// and the sites of the relations that connect them. A symbol stands at its
/// canonical site, but a function several files define stands at each
/// definition: a caller at the one its calls are written in, a callee at
/// every one.
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

    /// The definition whose calls the callees are, of a function several
    /// files define; every call of the root otherwise.
    std::optional<index::Site> body;
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

/// The items a hierarchy prepared at a cursor starts from, one per symbol
/// under it: functions, and the variables, fields and enumerators the
/// functions using them call; types for a type hierarchy. An item spans
/// the whole declaration and selects its name; its detail shows the
/// symbol's qualified name inside a namespace or a class, and a function's
/// signature; it carries the symbol's handle in `data` (see data_symbol).
/// It stands for the symbol, not the cursor: anchored at the symbol's
/// canonical site, expanding from a use renders the same root as expanding
/// from the declaration. A function several files define has a body in
/// each: a call hierarchy starts from the definition the cursor names,
/// else from every one. A symbol no source places (an implicitly declared
/// `operator delete`) has no item, nor has a function's local, which no
/// function but its own calls.
std::vector<protocol::CallHierarchyItem>
    call_hierarchy_items(const index::IndexQuery& query,
                         const index::IndexQuery::Cursor& cursor,
                         PositionEncoding encoding);
std::vector<protocol::TypeHierarchyItem>
    type_hierarchy_items(const index::IndexQuery& query,
                         const index::IndexQuery::Cursor& cursor,
                         PositionEncoding encoding);

/// The symbol a hierarchy item stands for: its handle when intact, else
/// the symbol at the item's recorded range (`at_range`) in the serving
/// source of `file`.
std::optional<index::SymbolHash> item_symbol(const index::IndexQuery& query,
                                             Fid file,
                                             std::optional<index::IndexQuery::Cursor> at_range,
                                             const std::optional<protocol::LSPAny>& data);

/// A symbol's neighbours in the call graph and the type hierarchy, asked
/// of every project knowing it (index::IndexQuery::asked): each project's
/// in the files it answers for, the same entity once.
std::vector<protocol::CallHierarchyIncomingCall> incoming_calls(const index::IndexQuery& query,
                                                                index::SymbolHash symbol,
                                                                Fid file,
                                                                PositionEncoding encoding);
/// The outgoing calls start from the body of the definition the item's
/// `name` site stands on, of a function several files define.
std::vector<protocol::CallHierarchyOutgoingCall>
    outgoing_calls(const index::IndexQuery& query,
                   index::SymbolHash symbol,
                   Fid file,
                   const std::optional<index::Site>& name,
                   PositionEncoding encoding);
std::vector<protocol::TypeHierarchyItem> supertypes(const index::IndexQuery& query,
                                                    index::SymbolHash symbol,
                                                    Fid file,
                                                    PositionEncoding encoding);
std::vector<protocol::TypeHierarchyItem> subtypes(const index::IndexQuery& query,
                                                  index::SymbolHash symbol,
                                                  Fid file,
                                                  PositionEncoding encoding);

}  // namespace clice::feature

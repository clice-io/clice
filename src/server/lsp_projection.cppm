module;

#include "modules/prelude.h"

module clice:server.lsp_projection;

import :index.navigation;
import :index.query;
import :index.site;
import :index.symbol;
import :semantic.symbol;

/// The LSP shapes of the index query's domain values: the one place that
/// spells URIs, maps sites through their coordinates and encodes symbol
/// handles for the protocol. Transports and feature assembly call these at
/// their reply edge; nothing below it sees a protocol type.
namespace clice::to_lsp {

namespace protocol = kota::ipc::protocol;

protocol::Range range(const index::Site& site);

protocol::Location location(const index::Site& site);

std::vector<protocol::Location> locations(llvm::ArrayRef<index::Site> sites);

std::vector<protocol::Range> ranges(llvm::ArrayRef<index::Site> sites);

std::vector<protocol::DocumentHighlight>
    document_highlights(llvm::ArrayRef<index::PlacedHighlight> highlights);

/// `container` is the qualified name of the symbol's parent, empty at the
/// translation unit. A function is named with its signature, which tells
/// its overloads apart.
protocol::SymbolInformation symbol_information(const index::SymbolRef& symbol,
                                               const index::Site& site,
                                               llvm::StringRef container);

/// Hierarchy items span the whole declaration (`extent`) and select its
/// name (`site`); the detail shows a symbol's `qualified` name inside a
/// namespace or a class, and a function's signature. They carry their symbol handle
/// in `data` as a decimal string: a raw 64-bit integer would be parsed into
/// a double by a JavaScript client and come back rounded.
protocol::CallHierarchyItem call_hierarchy_item(const index::SymbolRef& symbol,
                                                const index::Site& site,
                                                const index::Site& extent,
                                                llvm::StringRef qualified);
protocol::TypeHierarchyItem type_hierarchy_item(const index::SymbolRef& symbol,
                                                const index::Site& site,
                                                const index::Site& extent,
                                                llvm::StringRef qualified);

/// The symbol an item's `data` carries, if intact: a prepared hierarchy
/// item's, or a completion item's whose `#include` the server adds.
std::optional<index::SymbolHash> data_symbol(const std::optional<protocol::LSPAny>& data);

/// Whether a worker's raw reply is the JSON null or the empty array —
/// the only inspection ever made of a reply that is otherwise passed
/// through untouched.
bool is_null(const kota::codec::RawValue& raw);
bool is_empty(const kota::codec::RawValue& raw);

}  // namespace clice::to_lsp

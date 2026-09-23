#pragma once

#include <optional>
#include <vector>

#include "index/site.h"
#include "index/types.h"
#include "semantic/symbol.h"
#include "server/position.h"

#include "kota/codec/json/json.h"
#include "kota/ipc/lsp/protocol.h"
#include "llvm/ADT/ArrayRef.h"

/// The LSP shapes of the index query's domain values: the one place that
/// spells URIs, maps sites through their coordinates and encodes symbol
/// handles for the wire. Transports and feature assembly call these at
/// their reply edge; nothing below it sees a protocol type.
namespace clice::to_lsp {

namespace protocol = kota::ipc::protocol;

protocol::Range range(const index::Site& site);

protocol::Location location(const index::Site& site);

std::vector<protocol::Location> locations(llvm::ArrayRef<index::Site> sites);

std::vector<protocol::Range> ranges(llvm::ArrayRef<index::Site> sites);

/// The navigation surfaces' SymbolKind policy: the outline's exhaustive
/// table, with the kinds these surfaces display differently overridden.
protocol::SymbolKind symbol_kind(SymbolKind kind);

/// `container` is the qualified name of the symbol's parent, empty at the
/// translation unit.
protocol::SymbolInformation symbol_information(const index::SymbolRef& symbol,
                                               const index::Site& site,
                                               llvm::StringRef container);

/// Hierarchy items carry their symbol handle in `data` as a decimal
/// string: a raw 64-bit integer would be parsed into a double by a
/// JavaScript client and come back rounded.
protocol::CallHierarchyItem call_hierarchy_item(const index::SymbolRef& symbol,
                                                const index::Site& site);
protocol::TypeHierarchyItem type_hierarchy_item(const index::SymbolRef& symbol,
                                                const index::Site& site);

/// The symbol handle a prepared hierarchy item came back with, if intact.
std::optional<index::SymbolHash> hierarchy_symbol(const std::optional<protocol::LSPAny>& data);

/// Whether a worker's raw reply is the JSON null or the empty array —
/// the only inspection ever made of a reply that is otherwise passed
/// through untouched.
bool is_null(const kota::codec::RawValue& raw);
bool is_empty(const kota::codec::RawValue& raw);

}  // namespace clice::to_lsp

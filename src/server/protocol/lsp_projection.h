#pragma once

#include <optional>
#include <vector>

#include "index/types.h"
#include "semantic/symbol.h"
#include "server/protocol/position.h"

#include "kota/codec/json/json.h"
#include "kota/ipc/lsp/protocol.h"
#include "llvm/ADT/ArrayRef.h"

/// The LSP shapes of the index query's domain values: the one place that
/// spells URIs, maps sites through their coordinates and encodes symbol
/// handles for the wire. Transports and feature assembly call these at
/// their reply edge; nothing below it sees a protocol type.
namespace clice::to_lsp {

namespace protocol = kota::ipc::protocol;

/// A site's range in LSP positions; nullopt when its bytes fall outside
/// the coordinates' text (a row the index recorded past its content).
std::optional<protocol::Range> range(const Site& site);

std::optional<protocol::Location> location(const Site& site);

/// The locations of every mappable site, in order.
std::vector<protocol::Location> locations(llvm::ArrayRef<Site> sites);

/// The navigation surfaces' SymbolKind policy: the outline's exhaustive
/// table, with the kinds these surfaces display differently overridden.
protocol::SymbolKind symbol_kind(SymbolKind kind);

std::optional<protocol::SymbolInformation> symbol_information(const index::SymbolRef& symbol,
                                                              const Site& site);

/// Hierarchy items carry their symbol handle in `data` as a decimal
/// string: a raw 64-bit integer would be parsed into a double by a
/// JavaScript client and come back rounded.
std::optional<protocol::CallHierarchyItem> call_hierarchy_item(const index::SymbolRef& symbol,
                                                               const Site& site);
std::optional<protocol::TypeHierarchyItem> type_hierarchy_item(const index::SymbolRef& symbol,
                                                               const Site& site);

/// The symbol handle a prepared hierarchy item came back with, if intact.
std::optional<index::SymbolHash> hierarchy_symbol(const std::optional<protocol::LSPAny>& data);

/// Whether a worker's raw reply is the JSON null or the empty array —
/// the only inspection ever made of a reply that is otherwise passed
/// through untouched.
bool is_null(const kota::codec::RawValue& raw);
bool is_empty(const kota::codec::RawValue& raw);

}  // namespace clice::to_lsp

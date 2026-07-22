#pragma once

/// Shared JSON serialization helper for master and worker processes.

// kota/codec/json stays textual: its decode.h emits explicit template
// instantiations (over simdjson) that cannot be shared through a module.
// to_raw's template body needs to_json + lsp_config, so this header — and
// therefore its consumers — stays textual too: making it a module partition
// would leak those explicit instantiations into every clice importer and
// collide with the copy the zest test framework pulls in textually.
#include <utility>

#include "kota/codec/json/json.h"
#include "kota/ipc/codec/json.h"

namespace clice {

/// Serialize a value to JSON RawValue using LSP config.
template <typename T>
kota::codec::RawValue to_raw(const T& value) {
    auto json = kota::codec::json::to_json<kota::ipc::lsp_config>(value);
    return kota::codec::RawValue{json ? std::move(*json) : "null"};
}

}  // namespace clice

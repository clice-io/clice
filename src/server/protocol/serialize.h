#pragma once

/// Shared JSON serialization helper for master and worker processes.

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

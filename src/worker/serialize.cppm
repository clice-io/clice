module;

#include "modules/prelude.h"

/// Shared JSON serialization helper for master and worker processes.

#include "support/logging.macros.h"

module clice:worker.serialize;

import :support.logging;

namespace clice {

/// LSP's JSON, with text that is not UTF-8 — bytes a user's source may
/// hold — written as U+FFFD: what reaches the client must stay JSON it can
/// read.
struct client_json_config : kota::ipc::lsp_config {
    constexpr static auto invalid_utf8 = kota::codec::invalid_utf8::Replace;
};

/// `value` as the client's JSON, or `fallback` when it cannot be encoded.
template <typename T>
std::string to_client_json(const T& value, std::string_view fallback) {
    auto json = kota::codec::json::to_string<client_json_config>(value);
    if(!json) {
        LOG_ERROR("Cannot encode a reply as JSON: {}", json.error().to_string());
        return std::string(fallback);
    }
    return std::move(*json);
}

/// Serialize a value to a JSON RawValue for the client; null when it cannot
/// be encoded.
template <typename T>
kota::codec::RawValue to_raw(const T& value) {
    return kota::codec::RawValue{to_client_json(value, "null")};
}

}  // namespace clice

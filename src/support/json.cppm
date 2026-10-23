module;

#include "support/prelude.h"

#include "support/logging.macros.h"

module clice:support.json;

import :support.logging;

namespace clice {

/// JSON persisting paths. A path that is not UTF-8 is written with U+FFFD for
/// its bad bytes, as JSON holds only UTF-8: it then names no file, and only
/// what it describes is rebuilt, where a blob that cannot be written would
/// lose everything.
struct PathJsonConfig {
    constexpr static auto invalid_utf8 = codec::invalid_utf8::Replace;
};

/// LSP's JSON, with text that is not UTF-8 — bytes a user's source may
/// hold — written as U+FFFD: what reaches the client must stay JSON it can
/// read.
struct client_json_config : ipc::lsp_config {
    constexpr static auto invalid_utf8 = codec::invalid_utf8::Replace;
};

/// `value` as the client's JSON, or `fallback` when it cannot be encoded.
template <typename T>
std::string to_client_json(const T& value, std::string_view fallback) {
    auto json = json::to_string<client_json_config>(value);
    if(!json) {
        LOG_ERROR("Cannot encode a reply as JSON: {}", json.error().to_string());
        return std::string(fallback);
    }
    return std::move(*json);
}

/// Serialize a value to a JSON RawValue for the client; null when it cannot
/// be encoded.
template <typename T>
codec::RawValue to_raw(const T& value) {
    return codec::RawValue{to_client_json(value, "null")};
}

}  // namespace clice

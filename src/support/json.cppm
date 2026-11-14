module clice:support.json;

import :support.logging;
import :support.prelude;

namespace clice {

/// How JSON persisting paths is written. A path that is not UTF-8 is written
/// with U+FFFD for its bad bytes, as JSON holds only UTF-8: it then names no
/// file, and only what it describes is rebuilt, where a blob that cannot be
/// written would lose everything.
constexpr inline json::encode_options path_json{.invalid_utf8 = codec::invalid_utf8::Replace};

/// How the settings spell an enumerator of one word: its name in
/// lowercase (`Off` as `off`). Any other case names none, so a value is
/// written as the documentation lists it.
using LowercaseNames = codec::enum_names<kotatsu::naming::rename_policy::lower_snake>;

/// The settings' word for a number the machine picks: `"auto"`.
enum class Automatic : std::uint8_t { Auto };

/// The settings' word for no limit: `"unlimited"`.
enum class NoLimit : std::uint8_t { Unlimited };

/// A count the settings give, or leave to the machine.
using Count = std::variant<Automatic, std::uint32_t>;

/// A limit the settings give, or lift.
using Limit = std::variant<NoLimit, std::uint32_t>;

/// The most `limit` lets through.
inline std::uint32_t at_most(const Limit& limit) {
    auto* count = std::get_if<std::uint32_t>(&limit);
    return count ? *count : std::numeric_limits<std::uint32_t>::max();
}

/// `value` as the client's JSON, or `fallback` when it cannot be encoded.
/// Text that is not UTF-8 — bytes a user's source may hold — is written as
/// U+FFFD: what reaches the client must stay JSON it can read.
template <typename T>
std::string to_client_json(const T& value, std::string_view fallback) {
    auto json =
        json::to_string<ipc::lsp_format>(value, {.invalid_utf8 = codec::invalid_utf8::Replace});
    if(!json) {
        LOG_ERROR("Cannot encode a reply as JSON: {}", json.error().to_string());
        return std::string(fallback);
    }
    return std::move(*json);
}

/// Serialize a value to a JSON raw_value for the client; null when it cannot
/// be encoded.
template <typename T>
codec::raw_value to_raw(const T& value) {
    return codec::raw_value{to_client_json(value, "null")};
}

}  // namespace clice

/// The settings spell them in lowercase, as the documentation lists them.
template <>
struct kotatsu::codec::repr<clice::Automatic> : clice::LowercaseNames {};

template <>
struct kotatsu::codec::repr<clice::NoLimit> : clice::LowercaseNames {};

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace clice::ext {

struct ContextItem {
    std::string label;
    std::string description;

    /// Host source file (header contexts) or the file itself (source
    /// compile configurations).
    std::string uri;

    /// For header contexts: which include of the header in its direct
    /// includer this context represents (0-based, in directive order).
    /// Present only when the header is included more than once.
    std::optional<std::uint32_t> occurrence;

    /// For source compile configurations: canonical hash identifying the
    /// CDB entry. Pass it back in switchContext to select this entry.
    std::optional<std::string> command_hash;
};

struct QueryContextParams {
    std::string uri;
    std::optional<int> offset;
};

struct QueryContextResult {
    std::vector<ContextItem> contexts;
    int total = 0;
};

struct CurrentContextParams {
    std::string uri;
};

struct CurrentContextResult {
    std::optional<ContextItem> context;
};

struct SwitchContextParams {
    std::string uri;
    std::string context_uri;

    /// Include occurrence to pin (header contexts, 0-based).
    std::optional<std::uint32_t> occurrence;

    /// Canonical CDB entry hash to pin (source files with multiple
    /// compile commands).
    std::optional<std::string> command_hash;
};

struct SwitchContextResult {
    bool success = false;
};

}  // namespace clice::ext

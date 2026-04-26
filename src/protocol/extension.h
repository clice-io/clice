#pragma once

#include <optional>
#include <string>
#include <vector>

namespace clice::ext {

struct ContextItem {
    std::string label;
    std::string description;
    std::string uri;
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
};

struct SwitchContextResult {
    bool success = false;
};

}  // namespace clice::ext

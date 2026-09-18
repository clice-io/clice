#pragma once

#include <string>
#include <vector>

#include "kota/ipc/protocol.h"

namespace clice::control {

/// The control channel of a serving writer: what the commands that find
/// the index writer lock taken by a server ask of it. Paths are absolute
/// filesystem paths.

/// Sweep the build — every unit re-validated against the disk, the ones
/// the hash gate finds current skipped — and answer once the rows are
/// persisted. Refused when `configuration` is not the one the server
/// indexes, or when its configuration keeps background indexing off.
struct IndexParams {
    std::string configuration;
};

struct IndexResult {
    /// Units whose index attempt failed for good.
    std::vector<std::string> failed;
};

struct StatusParams {};

struct StatusResult {
    bool idle = true;
    int pending = 0;
    int total = 0;
    int indexed = 0;
};

}  // namespace clice::control

namespace kota::ipc::protocol {

template <>
struct RequestTraits<clice::control::IndexParams> {
    using Result = clice::control::IndexResult;
    constexpr inline static std::string_view method = "clice/index";
};

template <>
struct RequestTraits<clice::control::StatusParams> {
    using Result = clice::control::StatusResult;
    constexpr inline static std::string_view method = "clice/status";
};

}  // namespace kota::ipc::protocol

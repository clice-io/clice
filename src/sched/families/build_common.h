#pragma once

#include <format>
#include <initializer_list>
#include <string>

#include "worker/protocol.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/xxhash.h"

namespace clice {

/// Length-prefix each part (so embedded NULs cannot create colliding
/// splits) and return the 32-hex xxh3_128bits cache key.
///
/// FIXME: this concatenates all parts (including the preamble text, which
/// can be 10-100 KB) into a temporary std::string before hashing.  Use an
/// incremental xxh3 hasher to feed each StringRef directly and avoid the
/// large allocation on the ensure_pch hot path.
inline std::string cache_key(std::initializer_list<llvm::StringRef> parts) {
    std::string input;
    for(auto part: parts) {
        input += std::format("{}:", part.size());
        input += part;
    }
    auto hash = llvm::xxh3_128bits(llvm::arrayRefFromStringRef(input));
    return std::format("{:016x}{:016x}", hash.high64, hash.low64);
}

/// A PCH/PCM build failure is expected when the worker reported user-code
/// errors or the dispatch failed for an operational reason (memory-pressure
/// preemption, crash/restart window); anything else is clice breakage and is
/// reported as an anomaly.
inline bool expected_build_failure(const auto& result) {
    return result.has_value() ? result.value().has_user_errors
                              : worker::is_operational_error(result.error());
}

/// The error text of a failed build: the worker's message when it responded,
/// the dispatch error otherwise.
const inline std::string& build_failure_message(const auto& result) {
    return result.has_value() ? result.value().error : result.error().message;
}

}  // namespace clice

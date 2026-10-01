#pragma once

#include <cstdint>
#include <format>

#include "llvm/ADT/DenseMapInfo.h"

namespace clice {

/// A file id: the FileTable's compact handle for one interned path
/// spelling. A distinct type so fids, version ids and other integers
/// cannot mix silently. Default-constructed = invalid ("no file").
struct Fid {
    std::uint32_t raw = ~0u;

    constexpr bool valid() const {
        return raw != ~0u;
    }

    friend constexpr auto operator<=>(Fid, Fid) = default;
};

/// A version id: the FileTable's handle for one (file, content hash)
/// pair. Default-constructed = invalid ("no version").
struct VersionID {
    std::uint32_t raw = ~0u;

    constexpr bool valid() const {
        return raw != ~0u;
    }

    friend constexpr auto operator<=>(VersionID, VersionID) = default;
};

}  // namespace clice

template <>
struct llvm::DenseMapInfo<clice::Fid> {
    static unsigned getHashValue(clice::Fid fid) {
        return DenseMapInfo<std::uint32_t>::getHashValue(fid.raw);
    }

    static bool isEqual(clice::Fid lhs, clice::Fid rhs) {
        return lhs == rhs;
    }
};

template <>
struct llvm::DenseMapInfo<clice::VersionID> {
    static unsigned getHashValue(clice::VersionID vid) {
        return DenseMapInfo<std::uint32_t>::getHashValue(vid.raw);
    }

    static bool isEqual(clice::VersionID lhs, clice::VersionID rhs) {
        return lhs == rhs;
    }
};

/// Ids appear directly in log messages and test-failure output; format
/// as the raw id.
template <>
struct std::formatter<clice::Fid> : std::formatter<std::uint32_t> {
    auto format(clice::Fid fid, auto& ctx) const {
        return std::formatter<std::uint32_t>::format(fid.raw, ctx);
    }
};

template <>
struct std::formatter<clice::VersionID> : std::formatter<std::uint32_t> {
    auto format(clice::VersionID vid, auto& ctx) const {
        return std::formatter<std::uint32_t>::format(vid.raw, ctx);
    }
};

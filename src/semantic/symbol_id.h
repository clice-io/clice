#pragma once

#include <cstdint>
#include <string>

namespace clice::index {

/// An ID used to determine whether symbols from
/// different translation units are identical.
struct SymbolID {
    /// The hash value of the symbol's Unified Symbol Resolution (USR).
    std::uint64_t hash;

    /// The symbol name(not full qualified).
    std::string name;
};

}  // namespace clice::index

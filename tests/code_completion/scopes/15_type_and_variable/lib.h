#pragma once

namespace lib {

// A variable hides the class of its own name, whose constructors it
// declares.
inline constexpr struct length {
    constexpr length() = default;

    template <class T>
    constexpr explicit length(T) {}
} length;

}  // namespace lib

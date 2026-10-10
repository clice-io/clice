#pragma once

namespace shapes::detail {

#ifdef SHAPES_EXACT
inline double scale(double value) {
    return value;
}
#else
inline double scale(double value) {
    return static_cast<double>(static_cast<long>(value * 100)) / 100;
}
#endif

inline double square(double value) {
    return value * value;
}

}  // namespace shapes::detail

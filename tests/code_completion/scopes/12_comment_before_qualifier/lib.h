#pragma once

namespace lib {

struct Month {
    int value;
};

inline constexpr Month january{1};
inline constexpr Month february{2};

void tick();

}  // namespace lib

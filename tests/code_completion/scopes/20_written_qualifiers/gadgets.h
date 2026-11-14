#pragma once

namespace outer {
struct Gadget {};

namespace inner {
struct Widget {};
}  // namespace inner
}  // namespace outer

struct Local {
    static int vessel_spare;
};

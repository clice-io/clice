module;

#include "limits.h"

export module math;

export namespace math {

int §(add)add(int a, int b) {
    return clamp(a + b);
}

int twice(int a) {
    return §(call)add(a, a);
}

enum class Tone { tone_warm, tone_cool };
using enum Tone;

}  // namespace math

int hidden() {
    return 0;
}

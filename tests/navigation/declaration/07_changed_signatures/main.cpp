#include "geometry.h"

int §(changed)scale(long factor) {
    return static_cast<int>(factor * 2);
}

int §(overload)scale(double factor) {
    return static_cast<int>(factor * 2);
}

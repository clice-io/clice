#include "shapes/config.h"

#ifndef SHAPES_FAST
#error "fast.cpp is built with SHAPES_FAST set"
#endif

namespace shapes {

#if SHAPES_FAST
int fast_precision() {
    return shapes_precision;
}
#else
int exact_precision() {
    return shapes_precision;
}
#endif

}  // namespace shapes

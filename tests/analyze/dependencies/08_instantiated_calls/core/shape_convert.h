#pragma once
#include "core/convert.h"
#include "core/shape.h"

namespace ops {

template <>
inline int convert<Shape>(const Shape& shape) {
    return shape.sides;
}

}  // namespace ops

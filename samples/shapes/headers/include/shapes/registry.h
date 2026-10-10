#pragma once

namespace shapes {

class Shape;

int registry_count();

int registry_capacity();

void registry_add(const Shape& shape);

}  // namespace shapes

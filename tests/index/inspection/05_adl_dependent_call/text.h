#pragma once
#include "point.h"
namespace app {
struct Text { int size() const { return 0; } };
inline Text to_text(const Point&) { return {}; }
}

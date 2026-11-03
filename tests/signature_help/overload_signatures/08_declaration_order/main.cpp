#include "shapes.h"

struct Point {};

void trace(Point* point);

namespace draw {

void use(Circle* circle) {
    trace(§(global)circle);
    fill(§(namespaced)circle);
    stamp(§(templates)circle);
    blend(1, §(written)2);
}

}  // namespace draw

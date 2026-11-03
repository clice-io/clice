// Overloads that rank alike for the arguments written so far are listed in
// the order they are declared, whether a preamble header or the file itself
// declares them.
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

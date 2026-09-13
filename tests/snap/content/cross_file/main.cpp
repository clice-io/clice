// - verify: inspect

// Units across a header and its user: the partition of each file, the
// dependency edges of calls and member access, the redeclaration edges
// between a member declaration and its out-of-line definition, and the
// empty namespace as a unit of its own.
#include "shapes.h"

namespace app {

int area(const shapes::Rect& r) {
    return shapes::width(r) * r.height;
}

}  // namespace app

namespace unused {}

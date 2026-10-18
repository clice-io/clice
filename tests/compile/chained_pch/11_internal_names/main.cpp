#include "extra.h"

namespace {
struct Local {
    double §(local)radius;
};
}  // namespace

double run() {
    struct Scoped {
        double §(scoped)radius = 0;
    };
    double total = twice(Local{1.0}) + twice(Scoped{}) + make().§(made)value;
    total += §(helper)helper(§(scale)scale) + ring_twice() + hidden_twice();
    return total + Hidden{2.0}.§(hidden)radius + Ring{3.0}.§(ring)radius + §(ring_twice)ring_twice();
}

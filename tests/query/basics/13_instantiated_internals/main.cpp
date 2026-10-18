#include "box.h"

namespace {
struct Local {
    double radius;
};
}  // namespace

double local_twice() {
    return twice(Local{1.0});
}

double scoped_twice() {
    struct Scoped {
        double radius = 0;
    };
    return twice(Scoped{});
}

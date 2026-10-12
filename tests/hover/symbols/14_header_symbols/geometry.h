#pragma once

namespace geometry {

/// A point in the plane.
struct Point {
    int x;
    int y;
};

/// Distance from the origin, in taxicab steps.
int norm(Point p);

}

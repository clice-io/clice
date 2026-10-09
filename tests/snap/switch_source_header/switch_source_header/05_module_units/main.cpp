/// # Module interface and implementations
///
/// - status: supported
/// - verify: server
/// - indexing: true
///
/// A module interface unit pairs with the units implementing its module, and each of them with the interface

// switch: shapes.cppm
// switch: shapes_debug.cpp

import shapes;

int total(int width, int height) {
    return area(width, height) + perimeter(width, height);
}

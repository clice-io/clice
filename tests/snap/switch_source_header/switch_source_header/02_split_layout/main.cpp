/// # Headers apart from sources
///
/// - status: supported
/// - verify: server
/// - indexing: true
///
/// A header under `include/` finds its source under `src/`, ahead of another file of the same name that defines none of its declarations
///
/// Away from the header's own directory, a file of the same name counts
/// only when it includes the header: `tools/shape.cpp` does not, and is no
/// counterpart.

// switch: include/geometry/shape.h
// switch: src/shape.cpp

#include "include/geometry/shape.h"

int total(int width, int height) {
    return shape_area(width, height) + shape_perimeter(width, height);
}

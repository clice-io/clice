/// # Symbol Hierarchy
///
/// ## Nested symbol tree — symbols nest by their written scope; out-of-line definitions appear at their lexical position with qualified names
///
/// - status: supported
/// - order: 1

namespace demo {

struct Point {
    int x;
    int y;

    int manhattan() const;
};

int Point::manhattan() const {
    return x + y;
}

enum class Axis { X, Y };

int origin_distance(const Point& p);

namespace inner {
constexpr int level = 2;
}

}  // namespace demo

// A reopened namespace gets its own outline node per written scope.
namespace demo {
int reopened();
}

namespace demo::nested {
int compact();
}

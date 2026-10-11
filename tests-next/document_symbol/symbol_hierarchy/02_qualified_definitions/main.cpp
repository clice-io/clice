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

namespace demo {
int reopened();
}

namespace demo::nested {
int compact();
}

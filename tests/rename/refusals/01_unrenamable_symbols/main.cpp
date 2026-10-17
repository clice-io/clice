§(include)#include <cstdio>

#define §(macro_definition)TWICE(v) ((v) * 2)

struct Point {
    int x;
    Point §(operator)operator+(const Point& other) const { return {x + other.x}; }
};

template <typename T>
int width(const T& shape) {
    return shape.§(dependent)x;
}

int main() {
    Point p{1};
    std::§(system)printf("%d", (p + p).x);
    return §(macro_use)TWICE(p.x) + width(p);
}

§(include)#include <cstdio>

struct Point {
    int x;
    Point §(operator)operator+(const Point& other) const { return {x + other.x}; }
};

struct Size {
    int x;
};

template <typename T>
int width(const T& shape) {
    return shape.§(dependent)x;
}

int main() {
    Point p{1};
    std::§(system)printf("%d", (p + p).x);
    return §(command_line)FROM_BUILD + §(system_macro)EOF + width(p) + width(Size{2});
}

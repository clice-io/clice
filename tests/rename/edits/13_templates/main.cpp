struct Circle {
    double §(circle)area() const { return 3.0; }
};

struct Square {
    double §(square)area() const { return 4.0; }
};

struct Triangle {
    double area() const { return 2.0; }
};

template <typename T>
double measure(const T& shape) { return shape.area(); }

template <typename T>
double report(const T& shape) { return shape.area(); }

template <typename T>
struct Box {
    double §(box)area() const { return 1.0; }
};

template <typename T>
double size_of(const Box<T>& box) { return box.area(); }

int main() {
    return static_cast<int>(measure(Circle{}) + report(Square{}) + report(Triangle{}));
}

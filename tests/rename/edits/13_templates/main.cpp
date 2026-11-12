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
double measure(const T& shape) { return shape.§(measure_call)area(); }

template <typename T>
double report(const T& shape) { return shape.area(); }

template <typename T>
struct Box {
    double §(box)area() const { return 1.0; }
};

template <typename T>
double size_of(const Box<T>& box) { return box.§(size_of_call)area(); }

template <typename T>
struct Crate {
    static constexpr int §(crate)capacity = 1;
};

template <typename T>
int load() { return Crate<T>::§(load_call)capacity; }

struct Polygon {
    virtual int sides() const { return 0; }
};

struct Hexagon : Polygon {
    int sides() const override { return 6; }
};

template <typename T>
int count(const T& shape) { return shape.§(count_call)sides(); }

double §(scale)scale(double value) { return value * 2; }

template <typename T>
double grow(const T& value) { return scale(value); }

int main() {
    return static_cast<int>(measure(Circle{}) + report(Square{}) + report(Triangle{})) +
           count(Polygon{}) + count(Hexagon{}) + load<int>();
}

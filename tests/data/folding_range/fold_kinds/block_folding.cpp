/// # Block folding — functions, classes, structs, unions, enums, namespaces, lambdas
///
/// - status: supported
/// - order: 1

namespace geometry {

enum class Shape {
    Circle,
    Square,
    Triangle
};

struct Point {
    int x;
    int y;
};

union Value {
    int as_int;
    float as_float;
};

class Canvas {
    Point origin;

    int area() {
        auto scale = [](int factor) {
            return factor * 2;
        };
        return scale(4);
    }
};

}  // namespace geometry

// An exported class deriving from an imported one, overriding its virtual
// function.
export module Circle;
import Shape;

export class Circle : public §(base)Shape {
    int r;

public:
    Circle(int r) : r(r) {}

    int §(override)area() const override {
        return 3 * r * r;
    }
};

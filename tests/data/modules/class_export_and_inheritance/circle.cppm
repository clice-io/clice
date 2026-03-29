export module Circle;
import Shape;
export class Circle : public Shape {
    int r;
public:
    Circle(int r) : r(r) {}
    int area() const override { return 3 * r * r; }
};

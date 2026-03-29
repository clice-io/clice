export module Shape;
export class Shape {
public:
    virtual ~Shape() = default;
    virtual int area() const = 0;
};

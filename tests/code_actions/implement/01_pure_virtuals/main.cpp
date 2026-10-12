struct Shape {
    virtual ~Shape() = default;
    virtual double area() const = 0;
    virtual void scale(double factor) noexcept = 0;
    virtual const char* name() const { return "shape"; }
};

struct §(circle)Circle : Shape {
    double radius;
};

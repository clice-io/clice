/// # Implement pure virtual methods
///
/// - status: supported
/// - issues: clangd#1037
///
/// A class deriving from an abstract base receives an `override` declaration for each unimplemented pure virtual method
///
/// The declarations go at the end of the class body.

struct Shape {
    virtual ~Shape() = default;
    virtual double area() const = 0;
    virtual void scale(double factor) noexcept = 0;
    virtual const char* name() const { return "shape"; }
};

struct §(circle)Circle : Shape {
    double radius;
};

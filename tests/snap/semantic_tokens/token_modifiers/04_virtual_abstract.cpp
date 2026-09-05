/// # Virtual and abstract
///
/// - status: supported
///
/// Virtual methods, pure virtual methods and abstract classes

struct §Shape {
    virtual int §area();
    virtual int §perimeter() = 0;
    virtual ~Shape();
};

struct §Square : Shape {
    int §perimeter() override;
};

int measure(Shape& shape) {
    return shape.§area() + shape.§perimeter();
}

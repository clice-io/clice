/// # Token Modifiers
///
/// ## Virtual and abstract — virtual methods, pure virtual methods and abstract classes
///
/// - status: supported
/// - order: 4

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

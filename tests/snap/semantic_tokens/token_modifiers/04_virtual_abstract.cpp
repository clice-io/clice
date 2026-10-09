/// # Virtual and abstract
///
/// - status: supported
///
/// Virtual methods and abstract classes carry virtual or abstract modifiers

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

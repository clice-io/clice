struct §(Shape)Shape {
    virtual int §(area_decl)area();
    virtual int §(perimeter_decl)perimeter() = 0;
    virtual ~Shape();
};

struct §(Square)Square : Shape {
    int §(perimeter_decl2)perimeter() override;
};

int measure(Shape& shape) {
    return shape.§(area_use)area() + shape.§(perimeter_use)perimeter();
}

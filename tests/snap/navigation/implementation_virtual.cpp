/// - verify: server

struct §(base)Shape {
    virtual int §(base_method)area() = 0;
};

struct Circle : Shape {
    int §(override_def)area() override {
        return 3;
    }
};

struct Square : Shape {
    int area() override {
        return 4;
    }
};

int measure(Shape& shape) {
    return shape.§(virtual_call)area();
}

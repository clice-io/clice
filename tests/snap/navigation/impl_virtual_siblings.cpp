/// # Go to Implementation
///
/// ## Virtual method — every sibling override
///
/// - status: supported
/// - verify: server
/// - order: 2
///
/// Go-to-implementation on a virtual method lists every override across
/// the sibling derived classes.

struct Shape {
    virtual int §(base)area() = 0;
};

struct Circle : Shape {
    int area() override { return 1; }
};

struct Square : Shape {
    int area() override { return 2; }
};

struct Triangle : Shape {
    int area() override { return 3; }
};

struct §(class)Shape {
    Shape() : sides(0) {}
    explicit Shape(int n) : sides(n) {}
    §(destructor)~Shape() {}
    virtual int §(virtual)area() const { return 0; }
    int sides;
};

struct Square : Shape {
    §(constructor)Square() : Shape(4) {}
    int area() const override { return 1; }
};

template <typename T> struct §(template)Box { T value; };
template <> struct Box<bool> { bool flag; };
§(guide)Box(const char*) -> Box<bool>;

template <typename T> T §(function_template)pick(T value) { return value; }
template <> int pick<int>(int value) { return value + 1; }

int main() {
    Shape s(3);
    Square q;
    Box<int> b{1};
    Box<bool> f{true};
    return s.area() + q.area() + b.value + f.flag + pick(1) + pick<long>(2);
}

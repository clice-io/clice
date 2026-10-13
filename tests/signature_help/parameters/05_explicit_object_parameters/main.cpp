struct Shape {
    void move(this Shape& self, int dx, int dy);
};

void slide(Shape shape) {
    shape.move(§(first)1, 2);
    shape.move(1, §(second)2);
    auto scale = [](this auto self, double factor) { return factor; };
    scale(§(lambda)2.0);
}

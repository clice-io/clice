namespace shapes {
struct Circle {
    Circle(int radius);
};
}  // namespace shapes

struct Widget {
    Widget();
    Widget(int size);
    Widget(int width, int height);
};

template <typename T>
struct Box {
    Box(T value);
};

void build() {
    auto a = §(cast)Widget(1);
    auto b = §(temporary)Widget(2, 3);
    auto c = §(braced)Widget{4};
    auto d = §(empty)Widget();
    auto e = shapes::§(qualified)Circle(5);
    auto f = §(specialization)Box<int>(6);
}

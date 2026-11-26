struct Point {
    int x;
    int y;
};

struct Plain {
    int value;
};

struct Widget {
    Widget(int size);
};

template <typename T>
T* make(int size);

void build(§(parameter)Widget param) {
    auto a = §(aggregate)Point{1, 2};
    auto b = §(implicit)Plain();
    auto c = §(implicit_braced)Plain{};
    §(declarator)Widget d(1);
    auto e = static_cast<§(cast_type)Widget>(2);
    auto* f = make<§(argument)Widget>(3);
}

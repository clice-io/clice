// A local `auto` whose initializer depends on a function template's
// parameter hints the type the template's only instantiation gives it; a
// template instantiated with several types hints nothing.
struct Point {
    int x;
    int y;
};

template <typename T>
int copy(T value) {
    §(function)⟦auto kept = value;⟧
    return 0;
}

int copied = copy(Point{});

auto measure = [](const auto& point) {
    §(lambda)⟦auto width = point.x;⟧
    return width;
};

int measured = measure(Point{});

auto echo = [](auto value) {
    §(several)⟦auto kept = value;⟧
    return kept;
};

int first = echo(1);
long second = echo(2L);

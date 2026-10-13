// Locals of a template instantiated once, a class template's included: an
// `auto` copy takes the type the instantiation gives it, a range-for element
// the element of the range it gives.
struct Point {
    int x;
};

struct Points {
    Point* begin() const;
    Point* end() const;
};

template <typename T>
void walk(const T& points) {
    auto §(copy)first = points;
    for (auto §(element)point : points) {
    }
}

void use() {
    walk(Points{});
}

template <typename T>
struct Holder {
    void reset() {
        auto §(member)copy = T();
    }
};

template struct Holder<Point>;

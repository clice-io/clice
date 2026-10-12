struct Point {
    int x;
    int y;
};

template <class It, class F>
F for_each(It first, It last, F f) {
    for (; first != last; ++first) {
        f(*first);
    }
    return f;
}

void scale(Point* begin, Point* end) {
    auto norm = [](auto p) { return p.§(called); };
    norm(Point{});
    for_each(begin, end, [](auto& p) { p.§(algorithm); });
    auto by = [](const auto& l, const auto& r) { return l.§(two_parameters) < r.x; };
    by(*begin, *end);
}

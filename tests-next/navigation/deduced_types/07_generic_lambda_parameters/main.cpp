// A generic lambda's `auto` parameter takes the type of each call, yet
// inside the body its members go nowhere (clangd#493, clangd#1768).

struct Point {
    int x;
    int y;
    int norm() const;
};

template <typename It, typename F>
void each(It first, It last, F f) {
    for (; first != last; ++first) {
        f(*first);
    }
}

int use(Point* points, int count) {
    auto get = [](auto p) { return §(parameter)p.§(member)x; };
    each(points, points + count, [](const auto& p) { p.§(method)norm(); });
    return get(points[0]);
}

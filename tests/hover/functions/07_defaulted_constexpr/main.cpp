// A defaulted function shows the `constexpr` written on it, and none where
// it is not written.
struct Point {
    int x = 0;
    constexpr §(constructor)Point() = default;
    constexpr bool operator§(equal)==(const Point&) const = default;
};

struct Plain {
    int y = 0;
    §(unwritten)Plain() = default;
};

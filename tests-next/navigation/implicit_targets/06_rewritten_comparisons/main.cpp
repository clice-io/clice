namespace std {
struct strong_ordering {
    int n;
    constexpr operator int() const { return n; }
    static const strong_ordering equal, greater, less;
};
constexpr strong_ordering strong_ordering::equal = {0};
constexpr strong_ordering strong_ordering::greater = {1};
constexpr strong_ordering strong_ordering::less = {-1};
}

struct S {
    int value;
    bool operator==(const S& other) const;
    auto operator<=>(const S& other) const = default;
};

void use(S a, S b) {
    bool ne = a §(neq)!= b;
    bool gt = a §(gt)> b;
}

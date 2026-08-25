/// # Implicit Code Navigation
///
/// ## C++20 rewritten operators — navigate to the operator the rewrite uses
///
/// - status: supported
/// - verify: server
/// - order: 15
///
/// For a comparison synthesized by the C++20 rewrite rules, go-to-definition
/// on the written operator reaches the operator that actually implements it:
/// `!=` reaches `operator==`, and `>` reaches `operator<=>`.

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

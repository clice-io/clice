namespace std {
struct strong_ordering {
    int value;
    constexpr operator int() const { return value; }
    static const strong_ordering equal;
    static const strong_ordering greater;
    static const strong_ordering less;
};
constexpr strong_ordering strong_ordering::equal = {0};
constexpr strong_ordering strong_ordering::greater = {1};
constexpr strong_ordering strong_ordering::less = {-1};
}

struct Revision {
    int number;
    auto operator<=>(const Revision&) const = default;
};

bool changed = Revision{2} !§(01_spaceship_neq)= Revision{3};

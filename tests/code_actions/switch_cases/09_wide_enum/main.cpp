// The cases added keep the switch's meaning: an enumerator past 64 bits is
// its own value, not a truncated Low, and a section that falls through
// still falls through. The static_asserts check the behavior after the edit.

enum class Wide : __int128 { Low = 0, High = (__int128)1 << 64, Mid = 5 };

int level(Wide wide) {
    §(wide)switch (wide) {
    case Wide::Low:
        return 0;
    }
    return 1;
}

enum class Shape { Circle, Square, Triangle };

constexpr int sides(Shape shape) {
    int extra = 0;
    §(fallthrough)switch (shape) {
    case Shape::Circle:
        extra = 1;
        [[fallthrough]];
    case Shape::Square:
        int count = 4;
        return count + extra;
    }
    return 3;
}

static_assert(sides(Shape::Circle) == 5);
static_assert(sides(Shape::Triangle) == 3);

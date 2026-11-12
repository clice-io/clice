namespace §(namespace)math {
int one();
}

namespace math {
int two();
}

int math::one() { return 1; }

namespace algebra = math;
using namespace math;

enum class Color { §(enumerator)red, green };
enum §(enum)Mood { calm };

int red = 0;

Color pick() { return Color::red; }
Mood settle() { return calm; }

struct Point {
    int §(field)x;
    Point() : x(0) {}
};

int get(Point p) { return p.x; }
int Point::*member = &Point::x;

using §(alias)Size = unsigned;
Size count;
typedef Size Count;

template <typename T>
concept §(concept)Small = sizeof(T) < 4;

template <Small T>
void take(T);

void give(Small auto value);
static_assert(Small<char>);

void spin() {
§(label)again:
    goto again;
}

int §(dollar)my$count = 0;

int use() { return my$count + red + two() + algebra::one(); }

#include <console.h>

#define MAKE_GETTER int getter() { return 1; }

MAKE_GETTER

int compute(int x) { return x; }

struct Meters {
    double value;
    Meters operator+(Meters other) const { return {value + other.value}; }
    explicit operator double() const { return value; }
};

Meters operator""_m(long double v) { return {static_cast<double>(v)}; }

struct Seconds {
    double value;
};

template <typename T>
double read(const T& item) { return item.value; }

int main() {
    Meters total = 1.0_m + 2.0_m;
    print_line("total");
    double both = read(total) + read(Seconds{1.0});
    return compute(CONSOLE_WIDTH) + getter() + static_cast<int>(static_cast<double>(total) + both);
}

int level = BUILD_LEVEL;

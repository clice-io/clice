#include <console.h>

#define CALL compute(1)
#define MAKE_GETTER int getter() { return 1; }

MAKE_GETTER

int compute(int x) { return x; }

struct Meters {
    double value;
    Meters operator+(Meters other) const { return {value + other.value}; }
    explicit operator double() const { return value; }
};

Meters operator""_m(long double v) { return {static_cast<double>(v)}; }

int main() {
    Meters total = 1.0_m + 2.0_m;
    print_line("total");
    return CALL + getter() + static_cast<int>(static_cast<double>(total));
}

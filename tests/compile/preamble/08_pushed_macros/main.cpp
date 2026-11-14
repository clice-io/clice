#define min(a, b) ((a) < (b) ? (a) : (b))
#define LIMIT 4
#pragma push_macro("min")
#pragma push_macro("LIMIT")
#undef min
#undef LIMIT
#include "h.h"

int min(int a, int b) { return a < b ? a : b; }
#pragma pop_macro("min")
#pragma pop_macro("LIMIT")

int main() {
    return min(1, 2) + LIMIT + helper();
}

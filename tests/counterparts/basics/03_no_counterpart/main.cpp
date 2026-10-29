#include "math.h"

enum class Color {
#define COLOR(name) name,
#include "colors.def"
#undef COLOR
};

int twice_square(int value) {
    return twice(square(value));
}

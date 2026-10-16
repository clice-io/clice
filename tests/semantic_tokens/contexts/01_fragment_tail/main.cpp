enum class Color {
#include "colors.def"
};

#include "palette.h"

Color pick() {
    return Color::red;
}

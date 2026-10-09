/// # Files without a counterpart
///
/// - status: supported
/// - verify: server
/// - indexing: true
///
/// A header that defines everything it declares, and a `.def` fragment, have no counterpart

// switch: math.h
// switch: colors.def

#include "math.h"

enum class Color {
#define COLOR(name) name,
#include "colors.def"
#undef COLOR
};

int twice_square(int value) {
    return twice(square(value));
}

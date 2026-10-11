#include "feature/f.h"
#include "feature/dup.h"

int feature_hook() {
    Dup dup{};
    return dup.x;
}

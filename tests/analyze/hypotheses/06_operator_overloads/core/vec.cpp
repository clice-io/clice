#include "core/vec.h"

Vec operator+(Vec a, Vec b) {
    return {a.x + b.x};
}

Vec operator+(Vec a, int b) {
    return {a.x + b};
}

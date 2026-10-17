#include "geometry.h"

int §(changed)scale(long factor) {
    return static_cast<int>(factor * 2);
}

int §(overload)scale(double factor) {
    return static_cast<int>(factor * 2);
}

static int §(internal)checksum(const char* data, int size) {
    return data[0] + size;
}

template <typename T>
T §(template)area(T side) {
    return side * side;
}

static int measure(int size);

static int §(static_changed)measure(long size) {
    return static_cast<int>(size);
}

static int tally(int count);

namespace {
int §(anonymous)tally(long count) {
    return static_cast<int>(count);
}
}  // namespace

int use() {
    Mutex mutex;
    return checksum("x", 1);
}

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

int use() {
    Mutex mutex;
    return checksum("x", 1);
}

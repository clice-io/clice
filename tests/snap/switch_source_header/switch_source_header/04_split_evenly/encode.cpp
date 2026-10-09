#include "codec.h"

int encode(int value) {
    return value ^ 0x5a;
}

#include "../include/codec.h"

int encode(int value) {
    return value ^ 0x5a;
}

int decode(int value) {
    return value ^ 0x5a;
}

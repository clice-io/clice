#include "../include/codec.h"

int checksum(int value) {
    return value % 255;
}

int pack(int value) {
    return value << 1;
}

int unpack(int value) {
    return value >> 1;
}

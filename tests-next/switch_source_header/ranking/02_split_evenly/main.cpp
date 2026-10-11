#include "codec.h"

int round_trip(int value) {
    return decode(encode(value));
}

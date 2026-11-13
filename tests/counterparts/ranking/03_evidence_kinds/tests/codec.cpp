#include "../include/codec.h"

int check_round_trip() {
    return decode(encode(7)) == 7 ? 0 : 1;
}

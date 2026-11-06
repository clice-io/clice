#include "pack.h"
#include "unused.h"

struct Packet {
    char tag;
    int value;
};

static_assert(sizeof(Packet) == 5);

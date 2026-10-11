#pragma pack(push, 1)
#include "h.h"

struct S {
    char c;
    int i;
};
#pragma pack(pop)

static_assert(sizeof(S) == 5, "packed");

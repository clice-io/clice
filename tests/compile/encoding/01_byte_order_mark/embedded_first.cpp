constexpr unsigned char bytes[] = {
#embed "answer.h"
};

static_assert(sizeof(bytes) == 21);

#include "answer.h"

static_assert(ANSWER == 42);

#include "answer.h"

#if __has_embed("answer.h")
static_assert(ANSWER == 42);
#endif

constexpr unsigned char bytes[] = {
#embed "answer.h"
};

static_assert(sizeof(bytes) == 21);

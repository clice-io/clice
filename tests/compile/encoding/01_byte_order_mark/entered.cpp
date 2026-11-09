#include "answer.h"

#if __has_embed("answer.h")
static_assert(ANSWER == 42);
#endif

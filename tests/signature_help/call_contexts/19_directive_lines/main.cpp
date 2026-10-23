#include "part.h"§(include)

#define CHECK(left, right) ((left) > (right))

#if CHECK(1, §(condition)2)
#endif

#define PICK(first, second) second
#include PICK("absent.h", §(computed)"part.h")

int value = part(1);

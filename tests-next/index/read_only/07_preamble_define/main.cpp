#define §(limit)LIMIT 10
#include "header.h"

int twice(int x) {
    return add(x, x) + LIMIT;
}

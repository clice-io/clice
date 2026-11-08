#include "config.h"

int limit() { return LIMIT; }

#undef LIMIT
#define LIMIT 8

int small() { return LIMIT; }

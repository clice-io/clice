#include "limits.h"
#include "log.h"
#include "check_a.h"

#define DEBUG_TYPE "a"
int a = LIMIT + LOG() + CHECK;
#ifdef LIMIT
int b = LIMIT + 1;
#endif
#undef DEBUG_TYPE

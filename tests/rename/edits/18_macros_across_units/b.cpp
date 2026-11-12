#include "limits.h"
#include "log.h"
#include "check_b.h"

#define §(debug_type)DEBUG_TYPE "b"
#if defined(LIMIT) && LIMIT > 5
int c = §(limit)LIMIT + LOG() + §(check)CHECK;
#endif
#undef LIMIT

#define CREATE_TRACE_POINTS
#include "trace.h"
#undef §(undefined)CREATE_TRACE_POINTS
#define HAVE_BUILTIN(x) 1
#if §(tested)HAVE_BUILTIN(cpuid)
#endif
#include "trace.h"

int after = traced;

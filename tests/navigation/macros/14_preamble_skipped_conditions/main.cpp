// The skipped block's conditions test a macro of the preamble's header and
// nothing else names it: the PCH loads the name only when it is asked for.
#include "flags.h"

#if 0
#ifdef §(skipped)FEATURE_FLAG
int skipped;
#elif defined(§(defined)FEATURE_FLAG)
int other;
#endif
#endif

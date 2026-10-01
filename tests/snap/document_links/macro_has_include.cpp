// A __has_include wrapped in a macro links where its argument is spelled.
#define HAS(x) __has_include(x)

#if HAS(<header_a.h>)
#include "header_a.h"
#endif

#if HAS("header_b.h")
#endif

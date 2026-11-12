#if __has_include("header_c.h")
#include "header_c.h"
#endif

#if __has_include("header_c.h") && __has_include("header_d.h")
#include "header_d.h"
#endif

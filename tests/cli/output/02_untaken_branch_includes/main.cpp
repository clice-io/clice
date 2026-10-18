#if 0
#include "zero.h"
#endif
#ifdef NEVER_DEFINED
#include "undefined.h"
#endif
#if __has_include("guarded.h")
#include "guarded.h"
#endif
int main() { return 0; }

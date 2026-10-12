// The class comes from a header the preamble includes, and a conditional
// opens among the leading directives: asked from a definition inside it, the
// definitions there reorder only among themselves.

#include "s.h"
#if !defined(FEATURE_OFF)
void S::§(c)c() {}
void S::b() {}
#endif

void S::a() {}

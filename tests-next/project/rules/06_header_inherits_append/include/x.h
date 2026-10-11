// The rule matches src/ only: the header compiles with the macro its host's
// rule appends.
#ifndef BUILDING_LIB
#error missing BUILDING_LIB
#endif
inline int x() { return 0; }

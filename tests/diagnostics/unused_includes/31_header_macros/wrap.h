#pragma once
#define WRAP INNER
inline int wrapped() {
    return WRAP;
}
#ifdef HAVE_WIDE
using width = long;
#else
using width = int;
#endif

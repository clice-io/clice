// The header instantiates less<int> through check<int>. The editor's
// compile of main.cpp skips the header's function bodies; the background
// index compiles the file as a whole, which instantiates less<int> at the
// end, under main.cpp's request, and reports the warning on that call.
#include "cmp.h"

bool b = less(-1, 1u);

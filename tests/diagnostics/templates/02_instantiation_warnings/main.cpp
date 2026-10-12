// A warning from an instantiation lands on the request as is, without the
// "In template:" prefix an error gets.
#include "cmp.h"
bool b = less(-1, 1u);

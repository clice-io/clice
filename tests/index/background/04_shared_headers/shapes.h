#pragma once
#include "units.h"
#ifdef WIDE
using area_type = long;
#else
using §(area_type)area_type = int;
#endif
inline area_type §(area)area(meters w, meters h) { return w * h; }

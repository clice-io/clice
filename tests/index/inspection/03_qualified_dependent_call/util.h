#pragma once
#include "vec.h"
template <class C, class P> int remove_bad(C& c, P p) { return ns::erase_if(c, p); }

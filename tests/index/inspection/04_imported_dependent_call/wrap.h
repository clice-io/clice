#pragma once
#include "config.h"
template <class C, class P> int remove_worse(C& c, P p) { return lib::erase_if(c, p); }

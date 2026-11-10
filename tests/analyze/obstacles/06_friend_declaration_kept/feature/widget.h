#pragma once
#include "core/inspect.h"
struct Widget {
    friend int core::inspect(const Widget&);
private:
    int secret = 0;
};

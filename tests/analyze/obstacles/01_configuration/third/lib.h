#pragma once

struct Third {
    int v;
};

#ifdef THIRD_WIDE
using ThirdWord = long;
#endif

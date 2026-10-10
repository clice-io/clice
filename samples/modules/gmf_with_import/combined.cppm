module;
#include "util.h"
export module Combined;
import Base;

export int combined() {
    return base() + util_helper();
}

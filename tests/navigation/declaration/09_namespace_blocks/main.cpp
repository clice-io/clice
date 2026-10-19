#include "big.h"
#include "more.h"

namespace big {
int third();
}

namespace big {
int fourth();
}

int total() {
    return §(qualifier)big::first() + big::second() + big::third() + big::fourth();
}

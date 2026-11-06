#include "lib.h"
#include "alias.h"
#include "unused.h"

int use() {
    app::X x{1};
    return x.v;
}

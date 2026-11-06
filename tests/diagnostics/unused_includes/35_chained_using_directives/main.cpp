#include "lib.h"
#include "alias.h"
#include "prelude.h"
#include "unused.h"

int use() {
    X x{1};
    return x.v;
}

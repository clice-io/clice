#include "fwd.h"
void take(Foo* foo);
#include "a.h"
Foo global{1};
#include "b.h"
#include "unused.h"

int use() {
    return global.v + bar();
}

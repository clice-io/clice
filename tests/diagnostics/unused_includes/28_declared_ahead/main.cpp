#include "fwd.h"
void take(Foo* foo);
#include "foo.h"
#include "unused.h"

int use(Foo& foo) {
    return foo.v;
}

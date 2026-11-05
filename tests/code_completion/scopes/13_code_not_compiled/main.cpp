// Where the compile reads no code it completes nothing, and the list is
// whole: typing more brings nothing either.
#include "lib.h"

#define IGNORE(x)

void use() {
#if 0
    lib::§(inactive);
#endif
    IGNORE(lib::§(unexpanded));
}

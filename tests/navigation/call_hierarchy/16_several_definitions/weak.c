#include "child.h"

void child(void) {}

__attribute__((weak)) void §(weak)father(void) {
    child();
}

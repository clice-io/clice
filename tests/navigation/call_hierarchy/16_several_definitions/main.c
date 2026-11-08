#include "child.h"

void §(strong)father(void) {
    §(child_call)child();
}

int §(caller)main(void) {
    §(father_call)father();
    return 0;
}

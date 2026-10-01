// - verify: server
//
// Calls written in a fragment the function body includes land at the
// include in the caller's file: hierarchy ranges are positions in the
// caller's document.

int §(leaf)leaf(int value) {
    return value;
}

int §(big)big() {
    int total = 0;
#include "body.inc"
    total += leaf(3);
    return total;
}

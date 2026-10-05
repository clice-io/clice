// - verify: server
//
// The header's declarations are outside the open file's own rows; the
// implicit conversion reaches this file's index only through its call
// relation, and still carries its name.

#include "conv.h"

int §(caller)use_conv() {
    Conv c;
    int i = c;
    return i + c.get();
}

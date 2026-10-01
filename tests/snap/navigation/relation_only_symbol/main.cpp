// - verify: server
//
// The header's declarations are outside the open file's own rows; the
// implicit conversion and the overridden method reach this file's index
// only through relations, and still carry their names.

#include "conv.h"

int §(caller)use_conv() {
    Conv c;
    int i = c;
    return i + c.get();
}

struct Derived : Base {
    int §(override)run() const override;
};

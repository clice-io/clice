// A namespace's operators, and every overload of a name the code before the
// point uses one of; no operator is typed by its name.
#include "lib.h"

bool equal(lib::Vec v) {
    lib::scale(v, 2);
    return v == 1;
}

void use() {
    lib::§(qualified);
}

void unqualified() {
    oper§(unqualified);
}

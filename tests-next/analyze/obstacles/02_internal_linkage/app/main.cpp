// helper and limit are static, hidden in an anonymous namespace: each is
// local to one translation unit once core/str.h is a module interface.
#include "core/str.h"

int main() {
    return helper() + hidden() + uses_helper() + limit;
}

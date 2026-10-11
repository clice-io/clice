#include "bare.h"
int shared_counter = 0;
#include "utils.h"
#define X(name, code) inline int handle_##name() { return code; }
#include "errors.def"
#undef X

int main() {
    return bare() + next_id() + handle_ok();
}

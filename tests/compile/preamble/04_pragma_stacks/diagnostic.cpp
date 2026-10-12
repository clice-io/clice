#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-variable"
#include "h.h"

int main() {
    int unused = helper();
    return 0;
}
#pragma clang diagnostic pop

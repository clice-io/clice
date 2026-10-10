#include "lib.h"

#ifdef RELEASE
int release_only = lib();
#else
int debug_only = lib();
#endif

int main() {
    return 0;
}

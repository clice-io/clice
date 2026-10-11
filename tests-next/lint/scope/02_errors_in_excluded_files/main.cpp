// Pins a compiler error in a file a rule keeps out of lint: the code does not
// compile, so the error is reported all the same.
#include "vendor/broken.h"

int main() {
    return 0;
}

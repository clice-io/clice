// Pins compiler errors in files a rule keeps out of lint, one of them a
// warning that is an error by default: the code does not compile, so the
// errors are reported all the same.
#include "vendor/narrow.h"
#include "vendor/broken.h"

int main() {
    return 0;
}

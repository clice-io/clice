// A header missing after the first declaration is outside the preamble:
// the code after it is still checked.

int before = 0;

#include "missing.h"

int main() {
    return undeclared;
}

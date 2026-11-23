// A macro named like the variable it stands for: the module exports the
// variable, and the program reading the name still goes through the macro.
#include <alpha/alpha.h>

int counter = 1;

int main() {
    return counter - 1;
}

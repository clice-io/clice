// A wrapped header's guard macro is a macro like any other: the program
// reading it still sees it defined.
#include <alpha/alpha.h>

int main() {
    return alpha::Thing{ALPHA_H}.v;
}

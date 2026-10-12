// A local that shadows a global variable, or hides a same-named function, is
// the one candidate offered: no duplicate entry, not the hidden function.
int counter = 0;

void tally() {
    int counter = 1;
    int v = coun§(variable);
}

namespace ns {
int maximum(int a, int b);
}

using namespace ns;

void bar() {
    int maximum = 1;
    int v = maxi§(function);
}

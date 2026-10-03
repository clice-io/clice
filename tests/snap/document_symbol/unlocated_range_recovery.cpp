// - diagnostics: expected

// Error recovery leaves a bound of these declarations unlocated: the
// typeless range-for variable and the unclosed namespace span their names.

void f() {
    int v[3];
    for(x : v) {}
}

namespace a {
int x;

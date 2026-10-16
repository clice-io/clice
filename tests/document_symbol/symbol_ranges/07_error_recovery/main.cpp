// Error recovery leaves a bound of these declarations unlocated: the
// typeless range-for variable spans its name, the unclosed namespace runs
// to the end of the file. A binding of a value without parts has no type.

void f() {
    int v[3];
    for(x : v) {}
    auto [first, second] = 1;
}

namespace a {
int x;

#include "lib.h"
#include "shape.h"
struct Square : Shape {
    int area() const override { return 4; }
};
int main() { return lib_fn(); }

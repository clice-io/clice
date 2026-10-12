template <int N>
struct Deep {
    static const int value = Deep<N + 1>::value;
};
int depth = Deep<0>::value;
#define OP(x) x,
enum Op {
#include "ops.def"
};
int main() { return add; }

#include <vector>

#include "utils.h"

int main() {
    auto v = make_range(5);
    int s = sum(v);
    return s;
}

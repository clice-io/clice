#if __has_include("host.cpp.expect.h")
#include "host.cpp.expect.h"
#endif

#include <vector>

#include "util.h"

int main() {
    std::vector<int> values{util_add(1, 2)};
    return values.empty() ? 1 : 0;
}

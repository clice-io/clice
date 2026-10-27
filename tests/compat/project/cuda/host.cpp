#if __has_include("host.cpp.expect.h")
#include "host.cpp.expect.h"
#endif

#include <cuda/std/type_traits>
#include <vector>

#include "util.h"

static_assert(cuda::std::is_integral_v<int>);

int main() {
    std::vector<int> values{util_add(1, 2)};
    return values.empty() ? 1 : 0;
}

#if __has_include("main.cpp.expect.h")
#include "main.cpp.expect.h"
#endif

#include <cstdio>
#include <string>
#include <vector>

#include "util.h"

#ifndef COMPAT_DEFINE
#error "the build's own define is missing"
#endif

int main() {
    std::vector<std::string> names{"a", "b"};
    std::printf("%d %zu\n", util_add(1, 2), names.size());
    return 0;
}

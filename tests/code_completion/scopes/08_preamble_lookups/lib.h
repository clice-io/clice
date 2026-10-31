#pragma once

namespace lib {

int helper_value(int x);
static int helper_static(int x) {
    return x;
}
constexpr int helper_limit = 8;

namespace {
int helper_hidden(int x) {
    return x;
}
}  // namespace

namespace detail {
int helper_detail();
}  // namespace detail

}  // namespace lib

namespace app {
using lib::helper_value;
}  // namespace app

namespace inner = lib::detail;

#define HELPER_MACRO(x) (x)

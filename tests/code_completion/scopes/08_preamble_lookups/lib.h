#pragma once

int helper_global();

namespace lib {

int helper_value(int x);
static int helper_static(int x) {
    return x;
}
constexpr int helper_limit = 8;
struct helper_type {
    int helper_field;
};
enum Mode { helper_mode };

namespace {
int helper_hidden(int x) {
    return x;
}
}  // namespace

namespace detail {
int helper_detail();
}  // namespace detail

inline namespace v1 {
int helper_versioned();
}  // namespace v1

template <class T>
concept helper_concept = true;

}  // namespace lib

namespace app {
using lib::helper_value;
}  // namespace app

namespace nested {
using namespace lib;
}  // namespace nested

namespace inner = lib::detail;

#define HELPER_MACRO(x) (x)
#define HELPER_SPACE lib
#define HELPER_DETAIL detail
#define HELPER_ROOT

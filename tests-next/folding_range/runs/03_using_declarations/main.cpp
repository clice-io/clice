namespace lib {
struct vector {};
struct string {};
struct map {};
enum class color { red, green };
}  // namespace lib

using lib::vector;
using lib::string;
using lib::map;

using namespace lib;
using enum color;

using alias = lib::vector;
using other = lib::string;

void scoped() {
    using lib::vector;
    using lib::string;
}

struct Base {
    void f();
    void g();
};

struct Derived : Base {
    using Base::f;
    using Base::g;
};

template <typename T>
struct Forwarding : T {
    using T::f;
    using typename T::type;
};

namespace same_line {
using lib::vector; using lib::string;
using lib::map;
}  // namespace same_line

namespace closing {
using lib::vector;
using lib::string; }

#define USE(name) using lib::name;

namespace macros {
USE(vector)
USE(string)
}  // namespace macros

#define USE_TWO(first, second) using lib::first; using lib::second;

namespace pairs {
USE_TWO(vector, string)
USE_TWO(map, vector)
USE_TWO(string, map)
}  // namespace pairs

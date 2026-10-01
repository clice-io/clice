/// # `using` declaration blocks
///
/// - status: supported
///
/// Consecutive using declarations and directives fold below the first one
///
/// A blank line or any other line ends the run, and alias declarations do not
/// join one. Using declarations produced by macros fold at the invocations.

namespace lib {
struct vector {};
struct string {};
struct map {};
enum class color { red, green };
}  // namespace lib

using lib::vector;   // ┐
using lib::string;   // │ one run
using lib::map;      // ┘

using namespace lib; // ┐ directives and using-enum
using enum color;    // ┘ declarations join runs too

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

#define USE(name) using lib::name;

namespace macros {
USE(vector)
USE(string)
}  // namespace macros

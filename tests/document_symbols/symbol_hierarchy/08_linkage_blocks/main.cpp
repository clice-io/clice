// Declarations in extern "C" blocks list at the level of the block, using
// directives and declarations list nothing, each declarator of a
// declaration lists apart, and a declaration and its definition both list.

extern "C" {
void c_api(int handle);
}

extern "C" void single_c(int);

namespace lib {
struct Thing {};
}

using namespace lib;
using lib::Thing;

int first_value, second_value;

void declared_then_defined();
void declared_then_defined() {}

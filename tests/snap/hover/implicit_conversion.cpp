/// # Expression Context
///
/// ## Implicit conversions — argument converted to the parameter type
///
/// - status: supported
/// - order: 4
///
/// When an argument reaches a parameter through an implicit conversion, the
/// card notes the target type, for both built-in and user-defined
/// conversions.

namespace implicit_conversion {

struct Wrapper {
  Wrapper(int value);
};

void take_float(float x);
void take_wrapper(Wrapper w);

void demo() {
  int n = 0;
  take_float(§(01_arithmetic)n);
  take_wrapper(§(02_user_defined)n);
}

}

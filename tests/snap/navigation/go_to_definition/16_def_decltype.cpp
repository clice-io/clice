/// # `decltype` type navigation
///
/// - status: supported
/// - verify: server
///
/// Go-to-definition on `decltype` reaches the type its operand names, through
/// any `decltype` that type was itself declared with

struct Widget {};

Widget global;

decl§(operand)type(global) copy = global;
decl§(nested)type(copy) again = copy;

auto make() -> decl§(trailing)type(global);

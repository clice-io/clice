struct Widget {};

Widget global;

decl§(operand)type(global) copy = global;
decl§(nested)type(copy) again = copy;

auto make() -> decl§(trailing)type(global);

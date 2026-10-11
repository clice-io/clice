// References asked on an `auto` are the deduced type's; the keyword itself
// is no use of the type, whether it declares a variable or a parameter.

struct §(type)Widget {};

Widget make_widget();

void once(au§(parameter_auto)to value) {}

void use() {
    au§(variable_auto)to widget = make_widget();
    once(widget);
}

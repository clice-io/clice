/// # `auto`-deduced variables
///
/// - status: supported
/// - verify: server
///
/// Go-to-type-definition on a variable declared `auto` reaches its deduced
/// type, as on the `auto` keyword itself

struct Widget {};

Widget make_widget();

void probe() {
    au§(keyword)to §(variable)widget = make_widget();
    const auto& §(reference)ref = widget;
}

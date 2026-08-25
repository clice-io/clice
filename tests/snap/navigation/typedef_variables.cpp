/// # Go to Type Definition
///
/// ## Variables and parameters
///
/// - status: supported
/// - verify: server
/// - order: 1
///
/// Go-to-type-definition on a local variable or a parameter reaches the
/// definition of its type.

struct §(type)Widget {};

Widget make_widget();

int probe(Widget §(param)param) {
    Widget §(local)local = make_widget();
    return 0;
}

/// # Go to Type Definition
///
/// ## `auto`-deduced variables
///
/// - status: unsupported
/// - order: 3
///
/// Go-to-type-definition on an `auto`-deduced variable should reach the
/// deduced type's definition; today the variable carries no type relation,
/// so it returns nothing.

struct Widget {};

Widget make_widget();

void probe() {
    auto widget = make_widget();  // go-to-type-def on widget → Widget
}

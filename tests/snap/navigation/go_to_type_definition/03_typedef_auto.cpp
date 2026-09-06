/// # `auto`-deduced variables
///
/// - status: unsupported
///
/// Auto-deduced variables do not navigate to their deduced type definitions yet

struct Widget {};

Widget make_widget();

void probe() {
    auto widget = make_widget();  // go-to-type-def on widget → Widget
}

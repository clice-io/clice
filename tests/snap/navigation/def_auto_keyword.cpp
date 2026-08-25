/// # Go to Definition
///
/// ## `auto` keyword navigates to the deduced type
///
/// - status: unsupported
/// - order: 12
/// - issues: clangd#2055
///
/// Go-to-definition on the `auto` keyword should reach the type it was
/// deduced to; today it returns nothing.

struct Widget {};

Widget make_widget();

void use() {
    auto widget = make_widget(); // go-to-def on auto → Widget
}

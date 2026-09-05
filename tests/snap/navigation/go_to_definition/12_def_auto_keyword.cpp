/// # Deduced `auto` type navigation
///
/// - status: unsupported
/// - issues: clangd#2055
///
/// The `auto` keyword does not navigate to its deduced type yet

struct Widget {};

Widget make_widget();

void use() {
    auto widget = make_widget(); // go-to-def on auto → Widget
}

/// # Constructor calls
///
/// - status: supported
/// - verify: server
///
/// Parentheses and braces navigate to the selected constructor
///
/// Go-to-definition on the opening parenthesis or brace of a constructor
/// call reaches the constructor overload resolution selected, for both the
/// `T(args)` and `T{args}` forms.

struct Widget {
    Widget(int w, int h);
};

void build() {
    Widget a§(paren)(800, 600);
    Widget b§(brace){800, 600};
}

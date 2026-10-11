// Calls in the shapes editing meets most: through a pointer, after a nested
// call, at an argument that has a default, with explicit template arguments,
// building a temporary, the second argument of a call operator.

struct Widget {
    Widget(int width, int height);
    void resize(int width, int height);
};

struct Grid {
    int operator()(int row, int column);
};

void configure(int width, int height = 100, bool visible = true);
int pair(int a, int b);
template <typename T> T convert(double value);

void calls(Widget* widget, Grid grid) {
    widget->resize(§(through_pointer)1, 2);
    (void)pair(pair(1, 2), §(after_nested)3);
    configure(1, §(default_argument)200);
    (void)convert<int>(§(explicit_template)1.0);
    Widget local = Widget(§(temporary)1, 2);
    (void)grid(0, §(call_operator)1);
}

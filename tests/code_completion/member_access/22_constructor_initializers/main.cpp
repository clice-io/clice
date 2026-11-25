namespace other {
int width_limit;
}

/// What every widget is built on.
struct Base {
    Base(int seed);
};

struct Point {
    int x;
    int y;
};

/// A name a person reads.
struct Name {
    Name();
    Name(const char* text);
};

template <typename T>
struct Tagged {
    Tagged(T tag);
};

struct Widget : Base {
    int width;

    /// The title of the window.
    Name name;

    Point origin;

    Widget() : §(in_class) {}

    Widget(int seed) : Base(seed), §(after_base) {}

    Widget(long);
    Widget(short);
    Widget(char);
    Widget(float);
    Widget(double);
    Widget(unsigned);
};

struct Label : Tagged<int> {
    Label() : Tagged§(template_arguments_written)<int>(1) {}

    Label(int) : Tag§(template_base_braces){1} {}

    Label(long) : Tagged§(template_arguments_spaced) <int>(1) {}
};

Widget::Widget(long) : §(out_of_line) {}

Widget::Widget(double) : width(1), §(after_member) {}

Widget::Widget(short) : wid§(arguments_written)(1) {}

Widget::Widget(float) : Base(1), wid§(braces_written){1} {}

Widget::Widget(unsigned) : Base(1), wid§(braces_spaced) {1}, name{} {}

template <typename T>
struct Holder : T {
    T value;

    Holder() : §(dependent_base) {}
};

Widget::Widget(char) : wi§(typed) {}

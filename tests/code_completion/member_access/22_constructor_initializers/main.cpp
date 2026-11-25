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

template <typename T>
struct Outer {
    using Base = T;

    template <typename U>
    struct Inner {
        Inner(U value);
    };

    struct Derived : Base {
        int Base;

        Derived() : §(member_named_as_base) {}
    };
};

struct Wrapped : Outer<int>::Inner<double> {
    Wrapped() : Inn§(qualified_template_arguments)<double>(1) {}
};

/// What a field grows from.
struct Grain {
    Grain(int value);
};

struct Planted : Grain {
    /// A member named after its base.
    int Grain;

    Planted() : §(base_named_as_member) {}
};

namespace ns {
struct Seed {
    Seed(int value);
};
}  // namespace ns

struct Seeded : ns::Seed {
    Seeded() : n§(namespace_written)::Seed(1) {}
};

template <typename... Bases>
struct Mixed : Bases... {
    Mixed() : Bas§(pack_braces){}... {}
};

Widget::Widget(char) : wi§(typed) {}

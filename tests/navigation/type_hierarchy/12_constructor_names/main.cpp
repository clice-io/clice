struct Base {};

struct Widget : Base {
    §(declared)Widget(int size);
};

struct Derived : Widget {
    using Widget::Widget;
};

Widget make() {
    return §(called)Widget(1);
}

Derived inherit() {
    return §(inherited)Derived(2);
}

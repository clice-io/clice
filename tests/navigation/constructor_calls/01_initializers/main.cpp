struct Base {
    Base(int x);
};

struct Widget : Base {
    Widget(int width, int height);
    Widget(int size) : §(base)Base§(base_paren)(size) {}
    Widget() : §(delegating)Widget§(delegating_paren)(1, 2) {}
};

// The constructions and operator calls a function writes are among its
// outgoing calls.

struct Widget {
    Widget(int w, int h);
    ~Widget();
    Widget operator+(const Widget& other) const;
};

Widget §(caller)combine() {
    Widget a(1, 2);
    Widget b{3, 4};
    return a + b;
}

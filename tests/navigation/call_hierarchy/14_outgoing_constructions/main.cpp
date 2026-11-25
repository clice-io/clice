// The constructions and operator calls a function writes are among its
// outgoing calls, a construction at the class's name that calls the
// constructor, else at its parenthesis or brace.

struct Widget {
    Widget(int w, int h);
    ~Widget();
    Widget operator+(const Widget& other) const;
};

Widget §(caller)combine() {
    Widget a(1, 2);
    Widget b{3, 4};
    Widget c = Widget(5, 6);
    return a + b;
}

// An injected class name calling a constructor takes the constructor's token,
// as the class's name does outside the class.
struct Widget {
    Widget(int size);

    Widget create() {
        return §(Widget)Widget§(paren)(42);
    }
};

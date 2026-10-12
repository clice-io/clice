// An injected class name keeps its class token when used as a constructor.
struct Widget {
    Widget(int size);

    Widget create() {
        return §(Widget)Widget§(paren)(42);
    }
};

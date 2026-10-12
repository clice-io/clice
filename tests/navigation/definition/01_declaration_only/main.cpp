extern int threshold;

int probe(int value);

struct Screen {
    static const int margin = 4;
    virtual void refresh() = 0;
};

int watch(Screen& screen, int value) {
    screen.§(method)refresh();
    return §(function)probe(value) + §(variable)threshold + Screen::§(constant)margin;
}

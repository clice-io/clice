/// # Go to Definition
///
/// ## Declaration-only symbols navigate to their declaration
///
/// - status: supported
/// - verify: server
/// - order: 3
///
/// Symbols that carry only a declaration — pure virtuals, `extern`
/// variables, in-class static constants — resolve to that declaration
/// instead of returning nothing.

extern int threshold;

int probe(int value);

struct Screen {
    static const int margin = 4;
    virtual void refresh() = 0;
};

int watch(Screen& screen, int value) {
    screen.§(pure_use)refresh();
    return §(fn_use)probe(value) + §(var_use)threshold + Screen::§(const_use)margin;
}

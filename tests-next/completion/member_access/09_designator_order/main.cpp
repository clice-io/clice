// C++20 designators go in declaration order, each field once: the fields
// list in that order, and one already designated is not offered again.
struct Config {
    int width;
    int height;
    bool fullscreen;
};

Config first = { .§(order) };
Config next = { .width = 800, .§(after_width) };

// C++20 designators go in declaration order, each field once: the fields
// list in that order, and only those declared after the last one designated
// are offered.
struct Config {
    int width;
    int height;
    bool fullscreen;
};

Config first = { .§(order) };
Config next = { .width = 800, .§(after_width) };
Config last = { .height = 600, .§(after_height) };

int scale(int value, double factor);

#define OPEN {
Config braced = { .width{800}, .§(after_braced) };
Config computed = { .width = scale(1, .5), .§(after_call) };
Config copied = { .width = first.§(member_value) };
Config typed = { .width = 1, .§(before_height)height = 2 };

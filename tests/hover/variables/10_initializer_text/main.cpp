// Initializers as the source writes them: across lines, around comments,
// in raw strings and through a lambda, or left out when a directive splits
// them.

namespace initializer_text {
int §(spread)spread = 1 +
                     2 /* two */ +
                     3;

const char* §(raw)raw = R"(a "quoted"
line)";

#define TWICE(x) ((x) * 2)
int §(continued)continued = TWICE(4) \
    + 1;

auto §(lambda)square = [](int value) {
    return value * value;
};

struct Widget {
    int (*§(member_lambda)callback)(int) = [](int value) { return value + 1; };
};

int §(directive)configured[] = {
    1,
#if defined(EXTRA)
    2,
#endif
    3,
};
}

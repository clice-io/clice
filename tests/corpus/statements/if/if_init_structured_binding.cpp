// if with init-statement using structured bindings
namespace if_init_sb {

struct Pair {
    int first;
    int second;
};

Pair divide(int a, int b) {
    if(b == 0)
        return {0, 0};
    return {a / b, a % b};
}

int safe_divide(int a, int b) {
    if(auto [quot, rem] = divide(a, b); b != 0) {
        return quot;
    }
    return -1;
}

struct ParseResult {
    bool success;
    int value;
    const char* error;
};

ParseResult parse(int x) {
    if(x < 0)
        return {false, 0, "negative"};
    return {true, x, nullptr};
}

int try_parse(int x) {
    if(auto [ok, val, err] = parse(x); ok) {
        return val;
    }
    return 0;
}

void test() {
    [[maybe_unused]] int r1 = safe_divide(10, 3);
    [[maybe_unused]] int r2 = try_parse(42);
}

}  // namespace if_init_sb

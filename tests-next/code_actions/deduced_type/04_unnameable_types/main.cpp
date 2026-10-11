template <typename T>
void g(T value) {
    §(dependent)auto copy = value;
}

auto make_local() {
    struct Local {};
    return Local{};
}

struct Pair {
    int first;
    int second;
};

class Widget {
    struct Handle {};

public:
    static Handle open();
};

void f() {
    §(lambda)auto callback = [] {};
    §(local)auto local = make_local();
    §(private)auto handle = Widget::open();
    §(size)auto size = sizeof(int);
    §(binding)auto [first, second] = Pair{};
}

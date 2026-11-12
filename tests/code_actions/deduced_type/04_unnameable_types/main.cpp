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

template <int* P>
struct Slot {};

class Registry {
    static int count;

public:
    static auto slot() {
        return Slot<&count>{};
    }
};

void f() {
    §(lambda)auto callback = [] {};
    §(local)auto local = make_local();
    §(private)auto handle = Widget::open();
    §(size)auto size = sizeof(int);
    §(binding)auto [first, second] = Pair{};
    §(private_argument)auto slot = Registry::slot();
}

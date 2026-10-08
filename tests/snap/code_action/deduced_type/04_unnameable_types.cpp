/// # Unnameable types stay auto
///
/// - status: supported
///
/// Lambdas, dependent types, structured bindings and types the declaration cannot name are not expanded
///
/// A type cannot be named where it is local to another function, a member type the declaration has no access to, or the type of `sizeof` with no standard name for it declared yet (MSVC compatibility declares `size_t` implicitly).

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

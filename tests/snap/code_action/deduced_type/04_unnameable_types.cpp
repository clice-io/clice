/// # Unnameable types stay auto
///
/// - status: supported
///
/// Lambdas, dependent types and types the declaration cannot name are not expanded
///
/// A type cannot be named where it is local to another function, a member type the declaration has no access to, or the type of `sizeof` with no standard name for it declared yet.

template <typename T>
void g(T value) {
    §(dependent)auto copy = value;
}

auto make_local() {
    struct Local {};
    return Local{};
}

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
}

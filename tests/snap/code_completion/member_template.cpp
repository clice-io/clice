/// # Member Access
///
/// ## Members of an instantiated class template — the destructor label keeps the written template arguments
///
/// - status: supported
/// - order: 2

// error-ok: the member access expression is left dangling at the point.
template <typename T>
struct Box {
    T value;
};

void bar() {
    Box<int> b;
    b.§(pos)
}

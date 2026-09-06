/// # Instantiated class template members
///
/// - status: supported
/// - diagnostics: expected
///
/// The destructor label keeps the written template arguments

// The member access expression is left dangling at the point.
template <typename T>
struct Box {
    T value;
};

void bar() {
    Box<int> b;
    b.§(pos)
}

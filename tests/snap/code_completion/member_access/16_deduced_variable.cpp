/// # Deduced variable members
///
/// - status: supported
/// - diagnostics: expected
///
/// A variable declared `auto` from a dependent initializer completes the members of the class it deduces to
///
/// `auto& row = rows[0]; row.` lists the members of `Vec<T>`. The
/// declarator applies as in a real deduction: `const auto&` makes the
/// object const, and a by-value `auto` drops the initializer's const.

// The member accesses dangle; the statements stay semicolon-terminated so a
// later marker is not dragged into recovery.
template <typename T>
struct Vec {
    T& operator[](int index);
    const T& operator[](int index) const;

    void push_back(const T& value);
    int size() const;
};

template <typename T>
void bar(Vec<Vec<T>> rows, const Vec<Vec<T>>& fixed) {
    auto& row = rows[0];
    row.§(reference);
    const auto& view = rows[0];
    view.§(const_reference);
    auto copy = fixed[0];
    copy.§(value);
    auto&& forwarded = rows[0];
    forwarded.§(forwarding);
}

/// # Deduced variable members
///
/// - status: supported
/// - diagnostics: expected
///
/// A variable declared `auto` from a dependent initializer completes the members of the class it deduces to
///
/// `auto& row = rows[0]; row.` lists the members of `Vec<T>`. The
/// declarator applies as in a real deduction: `const auto&` makes the
/// object const, a by-value `auto` drops the initializer's const, `auto&&`
/// and `decltype(auto)` keep it, and `auto*` takes the pointee. From a data
/// member, `decltype(auto)` takes the type the member is declared with, or
/// with parentheses the const reference the expression is.

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
struct Grid {
    Vec<T> cells;
    Vec<T> spare[2];
};

template <typename T>
void bar(Vec<Vec<T>> rows,
         const Vec<Vec<T>>& fixed,
         Vec<Vec<T>*> pointers,
         const Grid<T>& grid) {
    auto& row = rows[0];
    row.§(reference);
    const auto& view = rows[0];
    view.§(const_reference);
    auto copy = fixed[0];
    copy.§(value);
    auto&& forwarded = fixed[0];
    forwarded.§(forwarding);
    decltype(auto) exact = fixed[0];
    exact.§(decltype_auto);
    auto* pointer = pointers[0];
    pointer->§(pointer);
    decltype(auto) declared = grid.cells;
    declared.§(decltype_member);
    decltype(auto) named = (grid.cells);
    named.§(decltype_parenthesized);
    auto decayed = grid.spare;
    decayed->§(decayed_array);
}

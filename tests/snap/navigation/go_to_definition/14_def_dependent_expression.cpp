/// # Dependent expression members
///
/// - status: supported
/// - verify: server
///
/// A member accessed on what a dependent subscript, call or `auto` variable evaluates to resolves to the member declared on the class template
///
/// Where the call has a `const` overload, the constness of the object picks
/// the one it names.

template <typename T>
struct Vec {
    T& operator[](int index);
    const T& operator[](int index) const;
    T& front();
    const T& front() const;
    void push(const T& value);
};

template <typename T>
void drain(Vec<Vec<T>> rows, T value) {
    rows[0].§(subscript)push(value);
    rows.§(overload)front().§(call)push(value);
    auto& row = rows[0];
    row.§(deduced)push(value);
}

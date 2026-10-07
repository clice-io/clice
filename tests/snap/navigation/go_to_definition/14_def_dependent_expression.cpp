/// # Dependent expression members
///
/// - status: supported
/// - verify: server
///
/// A member accessed on what a dependent subscript, call or `auto` variable evaluates to resolves to the member declared on the class template
///
/// Where the call has a `const` overload, the constness of the object picks
/// the one it names; overloads that differ in their parameters are all
/// listed.

template <typename T>
struct Vec {
    T& operator[](int index);
    const T& operator[](int index) const;
    T& front();
    const T& front() const;
    void push(const T& value);
    void take(int count);
    void take(long count) const;
    void pick();
    template <typename U>
    void pick() const;
};

template <typename T>
void drain(Vec<Vec<T>> rows, const Vec<Vec<T>>& fixed, T value) {
    rows[0].§(subscript)push(value);
    rows.§(overload)front().§(call)push(value);
    fixed.§(const_overload)front();
    rows.§(parameters)take(1);
    rows.template §(template_overload)pick<int>();
    auto& row = rows[0];
    row.§(deduced)push(value);
    auto copy(rows[0]);
    copy.§(direct_init)push(value);
}

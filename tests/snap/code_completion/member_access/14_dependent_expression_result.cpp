/// # Dependent expression results
///
/// - status: supported
/// - issues: clangd#443
/// - diagnostics: expected
///
/// A member access on what a subscript or a member call returns inside a template completes the members of the class it evaluates to
///
/// `rows[0].` on a `Vec<Vec<T>>` lists the members of `Vec<T>`, followed
/// through the container's `reference` alias the way the standard containers
/// declare it. Where a member has a `const` overload, the constness of the
/// object picks the one called, and `->` on a returned iterator reaches the
/// element.

// The member accesses dangle; the statements stay semicolon-terminated so a
// later marker is not dragged into recovery.
template <typename T>
struct Allocator {
    using value_type = T;
};

template <typename A>
struct AllocatorTraits {
    using value_type = typename A::value_type;
    using pointer = value_type*;
};

template <typename P>
struct Iterator {
    P operator->() const;
};

template <typename T, typename A = Allocator<T>>
struct Vec {
    using value_type = T;
    using reference = value_type&;
    using const_reference = const value_type&;
    using iterator = Iterator<typename AllocatorTraits<A>::pointer>;

    reference operator[](int index);
    const_reference operator[](int index) const;
    reference front();
    const_reference front() const;
    iterator begin();

    void push_back(const T& value);
    int size() const;
};

template <typename T>
void bar(Vec<Vec<T>> rows, const Vec<Vec<T>>& fixed) {
    rows[0].§(subscript);
    rows.front().§(call);
    fixed[0].§(const_object);
    rows.begin()->§(iterator);
}

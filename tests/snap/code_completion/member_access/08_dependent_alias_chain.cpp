/// # Dependent alias chain
///
/// - status: supported
/// - diagnostics: expected
///
/// An alias that itself names a dependent member type resolves through
/// every link of the chain

// The member access expression is left dangling at the point.
template <typename T>
struct Vec {
    using value_type = T;
    T* data;
    int size() const;
};

template <typename T>
struct Outer {
    using inner = Vec<T>;
    using value = typename inner::value_type;
};

template <typename T>
void bar() {
    typename Outer<Vec<T>>::value v;
    v.§(pos)
}

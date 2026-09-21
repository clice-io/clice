/// # Dependent pointee members
///
/// - status: supported
/// - diagnostics: expected
///
/// `->` on a pointer to a dependent member type completes the pointee's
/// members

// The member access expression is left dangling at the point.
template <typename T>
struct Vec {
    using value_type = T;
    T* data;
    int size() const;
};

template <typename T>
void bar() {
    typename Vec<Vec<T>>::value_type* p;
    p->§(pos)
}

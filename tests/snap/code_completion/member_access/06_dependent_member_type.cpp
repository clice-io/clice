/// # Dependent member type
///
/// - status: supported
/// - diagnostics: expected
///
/// A variable whose type is a member alias of a dependent specialization
/// completes the members of the class the alias stands for
///
/// The alias is resolved with the written template arguments substituted,
/// so `Vec<Vec<T>>::value_type` lists the members of `Vec<T>` rather than
/// nothing at all.

// The member access expression is left dangling at the point.
template <typename T>
struct Vec {
    using value_type = T;
    T* data;
    int size() const;
};

template <typename T>
void bar() {
    typename Vec<Vec<T>>::value_type v;
    v.§(pos)
}

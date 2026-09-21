/// # Dependent scope qualifier
///
/// - status: supported
/// - diagnostics: expected
///
/// `::` after a dependent member type lists that type's members, and after
/// a dependent specialization the members of its matching partial
/// specialization

// The qualified-ids are left dangling at the points.
template <typename T>
struct Vec {
    using value_type = T;
    static int capacity;
    int size() const;
};

template <typename T>
struct Traits {
    static int primary_only;
};

template <typename T>
struct Traits<T*> {
    static int pointer_only;
};

template <typename T>
void bar() {
    int a = Vec<Vec<T>>::value_type::§(nested);
    int b = Traits<T*>::§(partial);
}

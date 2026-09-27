/// # Dependent scope qualifier
///
/// - status: supported
/// - diagnostics: expected
///
/// `::` after a dependent member type lists that type's members, after a
/// dependent specialization the members of its matching partial
/// specialization, and after a dependent member enumeration its enumerators

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

template <typename T>
struct Modes {
    enum class Mode { Fast, Slow };
};

template <typename T>
void baz() {
    auto m = Modes<T>::Mode::§(enumeration);
}

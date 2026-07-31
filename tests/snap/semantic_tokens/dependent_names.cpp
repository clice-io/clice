/// # Declarations & References
///
/// ## Dependent names — resolved through the primary template where one is known
///
/// - status: partial
/// - issues: clangd#154, clangd#297
/// - order: 16
///
/// Dependent members of a known template (`Box<T>`) resolve to the primary
/// template's declarations and keep their kinds. Members of a bare template
/// parameter have no candidate declaration and currently get no token;
/// heuristic coloring for such names remains open.

template <typename T>
struct Box {
    using value_type = int;
    static void reset();
    int size() const;
};

template <typename T>
void resolved(Box<T> box) {
    typename Box<T>::§value_type item;
    Box<T>::§reset();
    box.§size();
}

template <typename T>
void unresolved(T value) {
    typename T::§value_type item;
    T::§reset();
    value.§size();
}

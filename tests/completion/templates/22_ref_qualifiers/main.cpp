// Overloads that differ only in their ref-qualifiers, as std::optional's
// value(): the one the object binds to decides the type.
template <class T>
struct Optional {
    T& value() &;
    const T& value() const&;
    T&& value() &&;
    const T&& value() const&&;
};

template <class T>
struct Wrap {
    T inner;
    int count;
};

template <class T>
Optional<Wrap<T>> make();

template <class T>
void walk(Optional<Wrap<T>> held, const Optional<Wrap<T>> fixed) {
    held.value().§(lvalue);
    fixed.value().§(const_lvalue);
    make<T>().value().§(rvalue);
}

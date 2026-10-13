// Overloads that differ only in their ref-qualifiers, as std::optional's
// value(): the one the object binds to decides the type.
struct Mutable {
    int mutable_member;
};

struct Constant {
    int constant_member;
};

struct Moved {
    int moved_member;
};

template <class T>
struct Optional {
    Mutable& value() &;
    const Constant& value() const&;
    Moved&& value() &&;
    const Moved&& value() const&&;
    T held;
};

template <class T>
Optional<T> make();

template <class T>
void walk(Optional<T> held, const Optional<T> fixed) {
    held.value().§(lvalue);
    fixed.value().§(const_lvalue);
    make<T>().value().§(rvalue);
}

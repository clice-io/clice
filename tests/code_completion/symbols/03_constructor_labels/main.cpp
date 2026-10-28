// Class template constructors and constructor templates complete as the bare
// class name, never a templated spelling; deduction guides, written or
// generated, have no name to complete.
template <typename T, typename U>
struct Bazzz {
    Bazzz() {}

    Bazzz(T x) {}

    Bazzz(T x, U y) {}
};

template <typename T>
Bazzz(T) -> Bazzz<T, int>;

namespace lib {
template <typename T>
struct Wrapper {
    template <typename U>
    Wrapper(U value);
};
}  // namespace lib

void bar() {
    Ba§(class_template);
}

void baz() {
    lib::Wra§(constructor_template);
}

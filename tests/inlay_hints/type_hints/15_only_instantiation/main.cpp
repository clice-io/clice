// A specialization the code only names, through a pointer, is no
// instantiation: Holder<char> stays the only one.
template <typename T>
struct Holder {
    void reset() {
        §(named)⟦auto copy = T();⟧
    }
};

template struct Holder<char>;

Holder<int>* elsewhere;

// Wrapped<int*> instantiates the partial specialization, not the primary
// template.
template <typename T>
struct Wrapped {
    void reset() {
        §(partial)⟦auto copy = T();⟧
    }
};

template <typename T>
struct Wrapped<T*> {};

template struct Wrapped<char>;
template struct Wrapped<int*>;

// A class defined outside the template it is nested in names the
// template's parameters through a list of its own.
template <typename T>
struct Outer {
    struct Inner;
};

template <typename T>
struct Outer<T>::Inner {
    void reset() {
        §(nested)⟦auto copy = T();⟧
    }
};

template struct Outer<char>;

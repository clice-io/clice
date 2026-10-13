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
// template; a partial specialization's own parameters stay unbound.
template <typename T>
struct Wrapped {
    void reset() {
        §(partial)⟦auto copy = T();⟧
    }
};

template <typename T>
struct Wrapped<T*> {
    struct Inner;
};

template <typename T>
struct Wrapped<T*>::Inner {
    void reset() {
        §(partial_member)⟦auto copy = T();⟧
    }
};

template struct Wrapped<char>;
template struct Wrapped<int*>;

// A class or function defined outside the templates it is nested in names
// their parameters through lists of its own, one per template. Member's
// pattern is never instantiated, only Outer<char>'s copy of it, so U stays
// unbound.
template <typename T>
struct Outer {
    struct Inner;

    template <typename U>
    struct Member;
};

template <typename T>
struct Outer<T>::Inner {
    void reset() {
        §(nested)⟦auto copy = T();⟧
    }
};

template struct Outer<char>;

template <typename T>
template <typename U>
struct Outer<T>::Member {
    void reset() {
        §(member_template)⟦auto copy = T();⟧
    }

    void clear();
};

template <typename T>
template <typename U>
void Outer<T>::Member<U>::clear() {
    §(levels)⟦auto outer = T();
    auto inner = U();⟧
}

Outer<char>::Member<int> member;

// Out of its class, a member of a template instantiated twice has no one
// type either.
template <typename T>
struct Twice {
    void reset();
};

template <typename T>
void Twice<T>::reset() {
    §(twice)⟦auto copy = T();⟧
}

template struct Twice<char>;
template struct Twice<int>;

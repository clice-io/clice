template <class T>
struct X {
    using type = T;
};

template <class T>
struct X<T*> {
    using other = T;
};

template <>
struct X<int>;

template <class T>
struct Holder {
    using alias = X<int>;
};

template <class T>
struct Twice : X<T>, X<T&> {
    typename Twice::§(ambiguous)type both;
};

template <class T>
void use() {
    typename X<T*>::§(partial)other kept;
    typename X<T*>::§(partial_missing)type missing;
    typename Holder<T>::alias::§(declared_only)type incomplete;
}
